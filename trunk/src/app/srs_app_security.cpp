//
// Copyright (c) 2013-2025 The SRS Authors
//
// SPDX-License-Identifier: MIT
//

#include <srs_app_security.hpp>

#include <srs_app_config.hpp>
#include <srs_kernel_error.hpp>

using namespace std;

ISrsSecurity::ISrsSecurity()
{
}

ISrsSecurity::~ISrsSecurity()
{
}

SrsSecurity::SrsSecurity()
{
    config_ = _srs_config;
}

SrsSecurity::~SrsSecurity()
{
    config_ = NULL;
}

srs_error_t SrsSecurity::check(SrsRtmpConnType type, string ip, ISrsRequest *req)
{
    srs_error_t err = srs_success;

    // allow all if security disabled.
    if (!config_->get_security_enabled(req->vhost_)) {
        return err; // OK
    }

    // rules to apply
    SrsConfDirective *rules = config_->get_security_rules(req->vhost_);
    return do_check(rules, type, ip, req);
}

srs_error_t SrsSecurity::do_check(SrsConfDirective *rules, SrsRtmpConnType type, string ip, ISrsRequest *req)
{
    srs_error_t err = srs_success;

    if (!rules) {
        return srs_error_new(ERROR_SYSTEM_SECURITY, "default deny for %s", ip.c_str());
    }

    // deny if matches deny strategy.
    if ((err = deny_check(rules, type, ip)) != srs_success) {
        return srs_error_wrap(err, "for %s", ip.c_str());
    }

    // allow if matches allow strategy.
    if ((err = allow_check(rules, type, ip)) != srs_success) {
        return srs_error_wrap(err, "for %s", ip.c_str());
    }

    return err;
}

srs_error_t SrsSecurity::allow_check(SrsConfDirective *rules, SrsRtmpConnType type, std::string ip)
{
    int allow_rules = 0;
    int deny_rules = 0;

    for (int i = 0; i < (int)rules->directives_.size(); i++) {
        SrsConfDirective *rule = rules->at(i);

        if (rule->name_ != "allow") {
            if (rule->name_ == "deny") {
                deny_rules++;
            }
            continue;
        }
        allow_rules++;

        string cidr_ipv4 = srs_net_get_cidr_ipv4(rule->arg1());
        string cidr_mask = srs_net_get_cidr_mask(rule->arg1());

        switch (type) {
        case SrsRtmpConnPlay:
        case SrsHlsPlay:
        case SrsFlvPlay:
        case SrsRtcConnPlay:
        case SrsSrtConnPlay:
            if (rule->arg0() != "play") {
                break;
            }
            if (rule->arg1() == "all" || rule->arg1() == ip) {
                return srs_success; // OK
            }
            if (srs_net_is_ipv4(cidr_ipv4) && cidr_mask != "" && srs_net_ipv4_within_mask(ip, cidr_ipv4, cidr_mask)) {
                return srs_success; // OK
            }
            break;
        case SrsRtmpConnFMLEPublish:
        case SrsRtmpConnFlashPublish:
        case SrsRtmpConnHaivisionPublish:
        case SrsRtcConnPublish:
        case SrsSrtConnPublish:
            if (rule->arg0() != "publish") {
                break;
            }
            if (rule->arg1() == "all" || rule->arg1() == ip) {
                return srs_success; // OK
            }
            if (srs_net_is_ipv4(cidr_ipv4) && cidr_mask != "" && srs_net_ipv4_within_mask(ip, cidr_ipv4, cidr_mask)) {
                return srs_success; // OK
            }
            break;
        case SrsRtmpConnUnknown:
        default:
            break;
        }
    }

    if (allow_rules > 0 || (deny_rules + allow_rules) == 0) {
        return srs_error_new(ERROR_SYSTEM_SECURITY_ALLOW, "not allowed by any of %d/%d rules", allow_rules, deny_rules);
    }
    return srs_success; // OK
}

srs_error_t SrsSecurity::deny_check(SrsConfDirective *rules, SrsRtmpConnType type, std::string ip)
{
    for (int i = 0; i < (int)rules->directives_.size(); i++) {
        SrsConfDirective *rule = rules->at(i);

        if (rule->name_ != "deny") {
            continue;
        }

        string cidr_ipv4 = srs_net_get_cidr_ipv4(rule->arg1());
        string cidr_mask = srs_net_get_cidr_mask(rule->arg1());

        switch (type) {
        case SrsRtmpConnPlay:
        case SrsHlsPlay:
        case SrsFlvPlay:
        case SrsRtcConnPlay:
        case SrsSrtConnPlay:
            if (rule->arg0() != "play") {
                break;
            }
            if (rule->arg1() == "all" || rule->arg1() == ip) {
                return srs_error_new(ERROR_SYSTEM_SECURITY_DENY, "deny by rule<%s>", rule->arg1().c_str());
            }
            if (srs_net_is_ipv4(cidr_ipv4) && cidr_mask != "" && srs_net_ipv4_within_mask(ip, cidr_ipv4, cidr_mask)) {
                return srs_error_new(ERROR_SYSTEM_SECURITY_DENY, "deny by rule<%s>", rule->arg1().c_str());
            }
            break;
        case SrsRtmpConnFMLEPublish:
        case SrsRtmpConnFlashPublish:
        case SrsRtmpConnHaivisionPublish:
        case SrsRtcConnPublish:
        case SrsSrtConnPublish:
            if (rule->arg0() != "publish") {
                break;
            }
            if (rule->arg1() == "all" || rule->arg1() == ip) {
                return srs_error_new(ERROR_SYSTEM_SECURITY_DENY, "deny by rule<%s>", rule->arg1().c_str());
            }
            if (srs_net_is_ipv4(cidr_ipv4) && cidr_mask != "" && srs_net_ipv4_within_mask(ip, cidr_ipv4, cidr_mask)) {
                return srs_error_new(ERROR_SYSTEM_SECURITY_DENY, "deny by rule<%s>", rule->arg1().c_str());
            }
            break;
        case SrsRtmpConnUnknown:
        default:
            break;
        }
    }

    return srs_success; // OK
}
