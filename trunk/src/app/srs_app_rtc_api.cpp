//
// Copyright (c) 2013-2025 The SRS Authors
//
// SPDX-License-Identifier: MIT
//

#include <srs_app_rtc_api.hpp>

#include <deque>
#include <srs_app_config.hpp>
#include <srs_app_http_api.hpp>
#include <srs_app_http_hooks.hpp>
#include <srs_app_rtc_conn.hpp>
#include <srs_app_rtc_server.hpp>
#include <srs_app_server.hpp>
#include <srs_app_statistic.hpp>
#include <srs_app_utility.hpp>
#include <srs_core_autofree.hpp>
#include <srs_protocol_json.hpp>
#include <srs_protocol_utility.hpp>
#include <unistd.h>
using namespace std;

// To limit the ICE ufrag/username to avoid unknown issue.
#define SRS_ICE_UFRAG_MIN 4
#define SRS_ICE_UFRAG_MAX 32
// STUN/ICE pwd should not be too short, browser will fail with error.
#define SRS_ICE_PWD_MIN 22
// To limit user to use too long password, to cause unknown issue.
#define SRS_ICE_PWD_MAX 32

SrsGoApiRtcPlay::SrsGoApiRtcPlay(ISrsRtcApiServer *server)
{
    server_ = server;
    security_ = new SrsSecurity();

    config_ = _srs_config;
    stat_ = _srs_stat;
    rtc_sources_ = _srs_rtc_sources;
    live_sources_ = _srs_sources;
    hooks_ = _srs_hooks;
}

SrsGoApiRtcPlay::~SrsGoApiRtcPlay()
{
    srs_freep(security_);

    config_ = NULL;
    stat_ = NULL;
    rtc_sources_ = NULL;
    live_sources_ = NULL;
    hooks_ = NULL;
}

// Request:
//      POST /rtc/v1/play/
//      {
//          "sdp":"offer...", "streamurl":"webrtc://r.ossrs.net/live/livestream",
//          "api":'http...", "clientip":"..."
//      }
// Response:
//      {"sdp":"answer...", "sid":"..."}
// @see https://github.com/rtcdn/rtcdn-draft
srs_error_t SrsGoApiRtcPlay::serve_http(ISrsHttpResponseWriter *w, ISrsHttpMessage *r)
{
    srs_error_t err = srs_success;

    SrsUniquePtr<SrsJsonObject> res(SrsJsonAny::object());

    if ((err = do_serve_http(w, r, res.get())) != srs_success) {
        srs_warn("RTC error %s", srs_error_desc(err).c_str());
        srs_freep(err);
        return srs_api_response_code(w, r, SRS_CONSTS_HTTP_BadRequest);
    }

    return srs_api_response(w, r, res->dumps());
}

// LCOV_EXCL_START
srs_error_t SrsGoApiRtcPlay::do_serve_http(ISrsHttpResponseWriter *w, ISrsHttpMessage *r, SrsJsonObject *res)
{
    srs_error_t err = srs_success;

    // For each RTC session, we use short-term HTTP connection.
    SrsHttpHeader *hdr = w->header();
    hdr->set("Connection", "Close");

    // Parse req, the request json object, from body.
    SrsJsonObject *req_raw = NULL;
    if (true) {
        string req_json;
        if ((err = r->body_read_all(req_json)) != srs_success) {
            return srs_error_wrap(err, "read body");
        }

        SrsJsonAny *json = SrsJsonAny::loads(req_json);
        if (!json || !json->is_object()) {
            return srs_error_new(ERROR_RTC_API_BODY, "invalid body %s", req_json.c_str());
        }

        req_raw = json->to_object();
    }
    SrsUniquePtr<SrsJsonObject> req(req_raw);

    // Fetch params from req object.
    SrsJsonAny *prop = NULL;
    if ((prop = req->ensure_property_string("sdp")) == NULL) {
        return srs_error_wrap(err, "not sdp");
    }
    string remote_sdp_str = prop->to_str();

    if ((prop = req->ensure_property_string("streamurl")) == NULL) {
        return srs_error_wrap(err, "not streamurl");
    }
    string streamurl = prop->to_str();

    string clientip;
    if ((prop = req->ensure_property_string("clientip")) != NULL) {
        clientip = prop->to_str();
    }
    if (clientip.empty()) {
        clientip = dynamic_cast<SrsHttpMessage *>(r)->connection()->remote_ip();
        // Overwrite by ip from proxy.
        string oip = srs_get_original_ip(r);
        if (!oip.empty()) {
            clientip = oip;
        }
    }

    string api;
    if ((prop = req->ensure_property_string("api")) != NULL) {
        api = prop->to_str();
    }

    string tid;
    if ((prop = req->ensure_property_string("tid")) != NULL) {
        tid = prop->to_str();
    }

    // The RTC user config object.
    SrsRtcUserConfig ruc;
    ruc.req_->ip_ = clientip;
    ruc.api_ = api;

    srs_net_url_parse_rtmp_url(streamurl, ruc.req_->tcUrl_, ruc.req_->stream_);

    srs_net_url_parse_tcurl(ruc.req_->tcUrl_, ruc.req_->schema_, ruc.req_->host_, ruc.req_->vhost_,
                            ruc.req_->app_, ruc.req_->stream_, ruc.req_->port_, ruc.req_->param_);

    // discovery vhost, resolve the vhost from config
    SrsConfDirective *parsed_vhost = config_->get_vhost(ruc.req_->vhost_);
    if (parsed_vhost) {
        ruc.req_->vhost_ = parsed_vhost->arg0();
    }

    // For client to specifies the candidate(EIP) of server.
    string eip = r->query_get("eip");
    if (eip.empty()) {
        eip = r->query_get("candidate");
    }
    string codec = r->query_get("codec");
    // For client to specifies whether encrypt by SRTP.
    string srtp = r->query_get("encrypt");
    string dtls = r->query_get("dtls");

    srs_trace(
        "RTC play %s, api=%s, tid=%s, clientip=%s, app=%s, stream=%s, offer=%dB, eip=%s, codec=%s, srtp=%s, dtls=%s",
        streamurl.c_str(), api.c_str(), tid.c_str(), clientip.c_str(), ruc.req_->app_.c_str(),
        ruc.req_->stream_.c_str(), remote_sdp_str.length(),
        eip.c_str(), codec.c_str(), srtp.c_str(), dtls.c_str());

    ruc.eip_ = eip;
    ruc.codec_ = codec;
    ruc.publish_ = false;
    ruc.dtls_ = (dtls != "false");

    if (srtp.empty()) {
        ruc.srtp_ = config_->get_rtc_server_encrypt();
    } else {
        ruc.srtp_ = (srtp != "false");
    }

    // TODO: FIXME: It seems remote_sdp doesn't represents the full SDP information.
    ruc.remote_sdp_str_ = remote_sdp_str;
    if ((err = ruc.remote_sdp_.parse(remote_sdp_str)) != srs_success) {
        return srs_error_wrap(err, "parse sdp failed: %s", remote_sdp_str.c_str());
    }

    if ((err = serve_http(w, r, &ruc)) != srs_success) {
        return srs_error_wrap(err, "serve");
    }

    res->set("code", SrsJsonAny::integer(ERROR_SUCCESS));
    res->set("server", SrsJsonAny::str(stat_->server_id().c_str()));
    res->set("service", SrsJsonAny::str(stat_->service_id().c_str()));
    res->set("pid", SrsJsonAny::str(stat_->service_pid().c_str()));

    // TODO: add candidates in response json?
    res->set("sdp", SrsJsonAny::str(ruc.local_sdp_str_.c_str()));
    res->set("sessionid", SrsJsonAny::str(ruc.session_id_.c_str()));

    return err;
}
// LCOV_EXCL_STOP

srs_error_t SrsGoApiRtcPlay::serve_http(ISrsHttpResponseWriter *w, ISrsHttpMessage *r, SrsRtcUserConfig *ruc)
{
    srs_error_t err = srs_success;

    if ((err = check_remote_sdp(ruc->remote_sdp_)) != srs_success) {
        return srs_error_wrap(err, "remote sdp check failed");
    }

    SrsSdp local_sdp;

    // Config for SDP and session.
    local_sdp.session_config_.dtls_role_ = config_->get_rtc_dtls_role(ruc->req_->vhost_);
    local_sdp.session_config_.dtls_version_ = config_->get_rtc_dtls_version(ruc->req_->vhost_);

    // Whether enabled.
    bool server_enabled = config_->get_rtc_server_enabled();
    bool rtc_enabled = config_->get_rtc_enabled(ruc->req_->vhost_);
    bool edge = config_->get_vhost_is_edge(ruc->req_->vhost_);

    if (rtc_enabled && edge) {
        rtc_enabled = false;
        srs_warn("disable WebRTC for edge vhost=%s", ruc->req_->vhost_.c_str());
    }

    if (server_enabled && !rtc_enabled) {
        srs_warn("RTC disabled in vhost %s", ruc->req_->vhost_.c_str());
    }
    if (!server_enabled || !rtc_enabled) {
        return srs_error_new(ERROR_RTC_DISABLED, "Disabled server=%d, rtc=%d, vhost=%s",
                             server_enabled, rtc_enabled, ruc->req_->vhost_.c_str());
    }

    // Whether RTC stream is active.
    bool is_rtc_stream_active = false;
    if (true) {
        SrsSharedPtr<SrsRtcSource> source = rtc_sources_->fetch(ruc->req_);
        is_rtc_stream_active = (source.get() && !source->can_publish());
    }

    // For RTMP to RTC, fail if disabled and RTMP is active, see https://github.com/ossrs/srs/issues/2728
    bool rtmp_to_rtc = config_->get_rtc_from_rtmp(ruc->req_->vhost_);
    if (rtmp_to_rtc && edge) {
        rtmp_to_rtc = false;
        srs_warn("disable RTMP to WebRTC for edge vhost=%s", ruc->req_->vhost_.c_str());
    }

    if (!is_rtc_stream_active && !rtmp_to_rtc) {
        SrsSharedPtr<SrsLiveSource> live_source = live_sources_->fetch(ruc->req_);
        if (live_source.get() && !live_source->inactive()) {
            return srs_error_new(ERROR_RTC_DISABLED, "Disabled rtmp_to_rtc of %s, see #2728", ruc->req_->vhost_.c_str());
        }
    }

    if ((err = security_->check(SrsRtcConnPlay, ruc->req_->ip_, ruc->req_)) != srs_success) {
        return srs_error_wrap(err, "RTC: security check");
    }

    if ((err = http_hooks_on_play(ruc->req_)) != srs_success) {
        return srs_error_wrap(err, "RTC: http_hooks_on_play");
    }

    // TODO: FIXME: When server enabled, but vhost disabled, should report error.
    ISrsRtcConnection *session = NULL;
    if ((err = server_->create_rtc_session(ruc, local_sdp, &session)) != srs_success) {
        return srs_error_wrap(err, "create session, dtls=%u, srtp=%u, eip=%s", ruc->dtls_, ruc->srtp_, ruc->eip_.c_str());
    }

    ostringstream os;
    if ((err = local_sdp.encode(os)) != srs_success) {
        return srs_error_wrap(err, "encode sdp");
    }

    string local_sdp_str = os.str();
    // Filter the \r\n to \\r\\n for JSON.
    string local_sdp_escaped = srs_strings_replace(local_sdp_str.c_str(), "\r\n", "\\r\\n");

    ruc->local_sdp_str_ = local_sdp_str;
    ruc->session_id_ = session->username();
    ruc->token_ = session->token();

    srs_trace("RTC username=%s, dtls=%u, srtp=%u, offer=%dB, answer=%dB", session->username().c_str(),
              ruc->dtls_, ruc->srtp_, ruc->remote_sdp_str_.length(), local_sdp_escaped.length());
    srs_trace("RTC remote offer: %s", srs_strings_replace(ruc->remote_sdp_str_.c_str(), "\r\n", "\\r\\n").c_str());
    srs_trace("RTC local answer: %s", local_sdp_escaped.c_str());

    return err;
}

srs_error_t SrsGoApiRtcPlay::check_remote_sdp(const SrsSdp &remote_sdp)
{
    srs_error_t err = srs_success;

    if (remote_sdp.group_policy_ != "BUNDLE") {
        return srs_error_new(ERROR_RTC_SDP_EXCHANGE, "now only support BUNDLE, group policy=%s", remote_sdp.group_policy_.c_str());
    }

    if (remote_sdp.media_descs_.empty()) {
        return srs_error_new(ERROR_RTC_SDP_EXCHANGE, "no media descriptions");
    }

    for (std::vector<SrsMediaDesc>::const_iterator iter = remote_sdp.media_descs_.begin(); iter != remote_sdp.media_descs_.end(); ++iter) {
        if (iter->type_ != "audio" && iter->type_ != "video") {
            return srs_error_new(ERROR_RTC_SDP_EXCHANGE, "unsupport media type=%s", iter->type_.c_str());
        }

        if (!iter->rtcp_mux_) {
            return srs_error_new(ERROR_RTC_SDP_EXCHANGE, "now only suppor rtcp-mux");
        }

        if (iter->sendonly_) {
            return srs_error_new(ERROR_RTC_SDP_EXCHANGE, "play API only support sendrecv/recvonly");
        }
    }

    return err;
}

srs_error_t SrsGoApiRtcPlay::http_hooks_on_play(ISrsRequest *req)
{
    srs_error_t err = srs_success;

    if (!config_->get_vhost_http_hooks_enabled(req->vhost_)) {
        return err;
    }

    // the http hooks will cause context switch,
    // so we must copy all hooks for the on_connect may freed.
    // @see https://github.com/ossrs/srs/issues/475
    vector<string> hooks;

    if (true) {
        SrsConfDirective *conf = config_->get_vhost_on_play(req->vhost_);

        if (!conf) {
            return err;
        }

        hooks = conf->args_;
    }

    for (int i = 0; i < (int)hooks.size(); i++) {
        std::string url = hooks.at(i);
        if ((err = hooks_->on_play(url, req)) != srs_success) {
            return srs_error_wrap(err, "on_play %s", url.c_str());
        }
    }

    return err;
}

SrsGoApiRtcPublish::SrsGoApiRtcPublish(ISrsRtcApiServer *server)
{
    server_ = server;
    security_ = new SrsSecurity();

    config_ = _srs_config;
    stat_ = _srs_stat;
    hooks_ = _srs_hooks;
}

SrsGoApiRtcPublish::~SrsGoApiRtcPublish()
{
    srs_freep(security_);

    config_ = NULL;
    stat_ = NULL;
    hooks_ = NULL;
}

// Request:
//      POST /rtc/v1/publish/
//      {
//          "sdp":"offer...", "streamurl":"webrtc://r.ossrs.net/live/livestream",
//          "api":'http...", "clientip":"..."
//      }
// Response:
//      {"sdp":"answer...", "sid":"..."}
// @see https://github.com/rtcdn/rtcdn-draft
srs_error_t SrsGoApiRtcPublish::serve_http(ISrsHttpResponseWriter *w, ISrsHttpMessage *r)
{
    srs_error_t err = srs_success;

    SrsUniquePtr<SrsJsonObject> res(SrsJsonAny::object());

    if ((err = do_serve_http(w, r, res.get())) != srs_success) {
        srs_warn("RTC error %s", srs_error_desc(err).c_str());
        srs_freep(err);
        return srs_api_response_code(w, r, SRS_CONSTS_HTTP_BadRequest);
    }

    return srs_api_response(w, r, res->dumps());
}

// LCOV_EXCL_START
srs_error_t SrsGoApiRtcPublish::do_serve_http(ISrsHttpResponseWriter *w, ISrsHttpMessage *r, SrsJsonObject *res)
{
    srs_error_t err = srs_success;

    // For each RTC session, we use short-term HTTP connection.
    w->header()->set("Connection", "Close");

    // Parse req, the request json object, from body.
    SrsJsonObject *req_raw = NULL;
    if (true) {
        string req_json;
        if ((err = r->body_read_all(req_json)) != srs_success) {
            return srs_error_wrap(err, "read body");
        }

        SrsJsonAny *json = SrsJsonAny::loads(req_json);
        if (!json || !json->is_object()) {
            return srs_error_new(ERROR_RTC_API_BODY, "invalid body %s", req_json.c_str());
        }

        req_raw = json->to_object();
    }
    SrsUniquePtr<SrsJsonObject> req(req_raw);

    // Fetch params from req object.
    SrsJsonAny *prop = NULL;
    if ((prop = req->ensure_property_string("sdp")) == NULL) {
        return srs_error_wrap(err, "not sdp");
    }
    string remote_sdp_str = prop->to_str();

    if ((prop = req->ensure_property_string("streamurl")) == NULL) {
        return srs_error_wrap(err, "not streamurl");
    }
    string streamurl = prop->to_str();

    string clientip;
    if ((prop = req->ensure_property_string("clientip")) != NULL) {
        clientip = prop->to_str();
    }
    if (clientip.empty()) {
        clientip = dynamic_cast<SrsHttpMessage *>(r)->connection()->remote_ip();
        // Overwrite by ip from proxy.
        string oip = srs_get_original_ip(r);
        if (!oip.empty()) {
            clientip = oip;
        }
    }

    string api;
    if ((prop = req->ensure_property_string("api")) != NULL) {
        api = prop->to_str();
    }

    string tid;
    if ((prop = req->ensure_property_string("tid")) != NULL) {
        tid = prop->to_str();
    }

    // The RTC user config object.
    SrsRtcUserConfig ruc;
    ruc.req_->ip_ = clientip;
    ruc.api_ = api;

    srs_net_url_parse_rtmp_url(streamurl, ruc.req_->tcUrl_, ruc.req_->stream_);
    srs_net_url_parse_tcurl(ruc.req_->tcUrl_, ruc.req_->schema_, ruc.req_->host_, ruc.req_->vhost_,
                            ruc.req_->app_, ruc.req_->stream_, ruc.req_->port_, ruc.req_->param_);

    // Identify WebRTC publisher by param upstream=rtc
    ruc.req_->param_ = srs_strings_trim_start(ruc.req_->param_ + "&upstream=rtc", "&");

    // discovery vhost, resolve the vhost from config
    SrsConfDirective *parsed_vhost = config_->get_vhost(ruc.req_->vhost_);
    if (parsed_vhost) {
        ruc.req_->vhost_ = parsed_vhost->arg0();
    }

    // For client to specifies the candidate(EIP) of server.
    string eip = r->query_get("eip");
    if (eip.empty()) {
        eip = r->query_get("candidate");
    }
    string codec = r->query_get("codec");

    srs_trace("RTC publish %s, api=%s, tid=%s, clientip=%s, app=%s, stream=%s, offer=%dB, eip=%s, codec=%s",
              streamurl.c_str(), api.c_str(), tid.c_str(), clientip.c_str(), ruc.req_->app_.c_str(), ruc.req_->stream_.c_str(),
              remote_sdp_str.length(), eip.c_str(), codec.c_str());

    ruc.eip_ = eip;
    ruc.codec_ = codec;
    ruc.publish_ = true;
    ruc.dtls_ = ruc.srtp_ = true;

    // TODO: FIXME: It seems remote_sdp doesn't represents the full SDP information.
    ruc.remote_sdp_str_ = remote_sdp_str;
    if ((err = ruc.remote_sdp_.parse(remote_sdp_str)) != srs_success) {
        return srs_error_wrap(err, "parse sdp failed: %s", remote_sdp_str.c_str());
    }

    if ((err = serve_http(w, r, &ruc)) != srs_success) {
        return srs_error_wrap(err, "serve");
    }

    res->set("code", SrsJsonAny::integer(ERROR_SUCCESS));
    res->set("server", SrsJsonAny::str(stat_->server_id().c_str()));
    res->set("service", SrsJsonAny::str(stat_->service_id().c_str()));
    res->set("pid", SrsJsonAny::str(stat_->service_pid().c_str()));

    // TODO: add candidates in response json?
    res->set("sdp", SrsJsonAny::str(ruc.local_sdp_str_.c_str()));
    res->set("sessionid", SrsJsonAny::str(ruc.session_id_.c_str()));

    return err;
}
// LCOV_EXCL_STOP

srs_error_t SrsGoApiRtcPublish::serve_http(ISrsHttpResponseWriter *w, ISrsHttpMessage *r, SrsRtcUserConfig *ruc)
{
    srs_error_t err = srs_success;

    if ((err = check_remote_sdp(ruc->remote_sdp_)) != srs_success) {
        return srs_error_wrap(err, "remote sdp check failed");
    }

    SrsSdp local_sdp;

    // TODO: FIXME: move to create_session.
    // Config for SDP and session.
    local_sdp.session_config_.dtls_role_ = config_->get_rtc_dtls_role(ruc->req_->vhost_);
    local_sdp.session_config_.dtls_version_ = config_->get_rtc_dtls_version(ruc->req_->vhost_);

    // Whether enabled.
    bool server_enabled = config_->get_rtc_server_enabled();
    bool rtc_enabled = config_->get_rtc_enabled(ruc->req_->vhost_);
    bool edge = config_->get_vhost_is_edge(ruc->req_->vhost_);

    if (rtc_enabled && edge) {
        rtc_enabled = false;
        srs_warn("disable WebRTC for edge vhost=%s", ruc->req_->vhost_.c_str());
    }

    if (server_enabled && !rtc_enabled) {
        srs_warn("RTC disabled in vhost %s", ruc->req_->vhost_.c_str());
    }
    if (!server_enabled || !rtc_enabled) {
        return srs_error_new(ERROR_RTC_DISABLED, "Disabled server=%d, rtc=%d, vhost=%s",
                             server_enabled, rtc_enabled, ruc->req_->vhost_.c_str());
    }

    // TODO: FIXME: When server enabled, but vhost disabled, should report error.
    // We must do stat the client before hooks, because hooks depends on it.
    ISrsRtcConnection *session = NULL;
    if ((err = server_->create_rtc_session(ruc, local_sdp, &session)) != srs_success) {
        return srs_error_wrap(err, "create session");
    }

    if ((err = security_->check(SrsRtcConnPublish, ruc->req_->ip_, ruc->req_)) != srs_success) {
        return srs_error_wrap(err, "RTC: security check");
    }

    // We must do hook after stat, because depends on it.
    if ((err = http_hooks_on_publish(ruc->req_)) != srs_success) {
        return srs_error_wrap(err, "RTC: http_hooks_on_publish");
    }

    ostringstream os;
    if ((err = local_sdp.encode(os)) != srs_success) {
        return srs_error_wrap(err, "encode sdp");
    }

    string local_sdp_str = os.str();
    // Filter the \r\n to \\r\\n for JSON.
    string local_sdp_escaped = srs_strings_replace(local_sdp_str.c_str(), "\r\n", "\\r\\n");

    ruc->local_sdp_str_ = local_sdp_str;
    ruc->session_id_ = session->username();
    ruc->token_ = session->token();

    srs_trace("RTC username=%s, offer=%dB, answer=%dB", session->username().c_str(),
              ruc->remote_sdp_str_.length(), local_sdp_escaped.length());
    srs_trace("RTC remote offer: %s", srs_strings_replace(ruc->remote_sdp_str_.c_str(), "\r\n", "\\r\\n").c_str());
    srs_trace("RTC local answer: %s", local_sdp_escaped.c_str());

    return err;
}

srs_error_t SrsGoApiRtcPublish::check_remote_sdp(const SrsSdp &remote_sdp)
{
    srs_error_t err = srs_success;

    if (remote_sdp.group_policy_ != "BUNDLE") {
        return srs_error_new(ERROR_RTC_SDP_EXCHANGE, "now only support BUNDLE, group policy=%s", remote_sdp.group_policy_.c_str());
    }

    if (remote_sdp.media_descs_.empty()) {
        return srs_error_new(ERROR_RTC_SDP_EXCHANGE, "no media descriptions");
    }

    for (std::vector<SrsMediaDesc>::const_iterator iter = remote_sdp.media_descs_.begin(); iter != remote_sdp.media_descs_.end(); ++iter) {
        if (iter->type_ != "audio" && iter->type_ != "video") {
            return srs_error_new(ERROR_RTC_SDP_EXCHANGE, "unsupport media type=%s", iter->type_.c_str());
        }

        if (!iter->rtcp_mux_) {
            return srs_error_new(ERROR_RTC_SDP_EXCHANGE, "now only suppor rtcp-mux");
        }

        if (iter->recvonly_) {
            return srs_error_new(ERROR_RTC_SDP_EXCHANGE, "publish API only support sendrecv/sendonly");
        }
    }

    return err;
}

srs_error_t SrsGoApiRtcPublish::http_hooks_on_publish(ISrsRequest *req)
{
    srs_error_t err = srs_success;

    if (!config_->get_vhost_http_hooks_enabled(req->vhost_)) {
        return err;
    }

    // the http hooks will cause context switch,
    // so we must copy all hooks for the on_connect may freed.
    // @see https://github.com/ossrs/srs/issues/475
    vector<string> hooks;

    if (true) {
        SrsConfDirective *conf = config_->get_vhost_on_publish(req->vhost_);
        if (!conf) {
            return err;
        }
        hooks = conf->args_;
    }

    for (int i = 0; i < (int)hooks.size(); i++) {
        std::string url = hooks.at(i);
        if ((err = hooks_->on_publish(url, req)) != srs_success) {
            return srs_error_wrap(err, "rtmp on_publish %s", url.c_str());
        }
    }

    return err;
}

SrsGoApiRtcWhip::SrsGoApiRtcWhip(ISrsRtcApiServer *server)
{
    server_ = server;
    publish_ = new SrsGoApiRtcPublish(server);
    play_ = new SrsGoApiRtcPlay(server);

    config_ = _srs_config;
}

SrsGoApiRtcWhip::~SrsGoApiRtcWhip()
{
    srs_freep(publish_);
    srs_freep(play_);

    config_ = NULL;
}

srs_error_t SrsGoApiRtcWhip::serve_http(ISrsHttpResponseWriter *w, ISrsHttpMessage *r)
{
    srs_error_t err = srs_success;

    // For each RTC session, we use short-term HTTP connection.
    w->header()->set("Connection", "Close");

    // Client stop publish.
    // TODO: FIXME: Stop and cleanup the RTC session.
    if (r->method() == SRS_CONSTS_HTTP_DELETE) {
        string username = r->query_get("session");
        string token = r->query_get("token");
        if (token.empty()) {
            return srs_error_new(ERROR_RTC_INVALID_SESSION, "token empty");
        }

        ISrsRtcConnection *session = server_->find_rtc_session_by_username(username);
        if (session && token != session->token()) {
            return srs_error_new(ERROR_RTC_INVALID_SESSION, "token %s not match", token.c_str());
        }

        if (session)
            session->expire();
        srs_trace("WHIP: Delete session=%s, p=%p, url=%s", username.c_str(), session, r->url().c_str());

        w->header()->set_content_length(0);
        w->write_header(SRS_CONSTS_HTTP_OK);
        return w->write(NULL, 0);
    }

    SrsRtcUserConfig ruc;
    if ((err = do_serve_http(w, r, &ruc)) != srs_success) {
        return srs_error_wrap(err, "serve");
    }
    if (ruc.local_sdp_str_.empty()) {
        return srs_go_http_error(w, SRS_CONSTS_HTTP_InternalServerError);
    }

    // The SDP to response.
    string sdp = ruc.local_sdp_str_;

    // Setup the content type to SDP.
    w->header()->set("Content-Type", "application/sdp");
    // The location for DELETE resource, not required by SRS, but required by WHIP.
    w->header()->set("Location", srs_fmt_sprintf("/rtc/v1/whip/?action=delete&token=%s&app=%s&stream=%s&session=%s",
                                                 ruc.token_.c_str(), ruc.req_->app_.c_str(), ruc.req_->stream_.c_str(), ruc.session_id_.c_str()));
    w->header()->set_content_length((int64_t)sdp.length());
    // Must be 201, see https://datatracker.ietf.org/doc/draft-ietf-wish-whip/
    w->write_header(201);

    // Response the SDP content.
    return w->write((char *)sdp.data(), (int)sdp.length());
}

srs_error_t SrsGoApiRtcWhip::do_serve_http(ISrsHttpResponseWriter *w, ISrsHttpMessage *r, SrsRtcUserConfig *ruc)
{
    srs_error_t err = srs_success;

    string remote_sdp_str;
    if ((err = r->body_read_all(remote_sdp_str)) != srs_success) {
        return srs_error_wrap(err, "read sdp");
    }

    string clientip;
    if (clientip.empty()) {
        clientip = dynamic_cast<SrsHttpMessage *>(r)->connection()->remote_ip();
        // Overwrite by ip from proxy.
        string oip = srs_get_original_ip(r);
        if (!oip.empty()) {
            clientip = oip;
        }
    }

    // For client to specifies the candidate(EIP) of server.
    string eip = r->query_get("eip");
    if (eip.empty()) {
        eip = r->query_get("candidate");
    }
    string codec = r->query_get("codec");
    string app = r->query_get("app");
    string stream = r->query_get("stream");
    string action = r->query_get("action");
    if (action.empty()) {
        action = "publish";
    }
    // For whip-play or whep, parsed to https://datatracker.ietf.org/doc/draft-murillo-whep/
    if (srs_strings_ends_with(r->path(), "/whip-play/") || srs_strings_ends_with(r->path(), "/whep/")) {
        action = "play";
    }

    // The RTC user config object.
    ruc->req_->ip_ = clientip;
    ruc->req_->host_ = r->host();
    ruc->req_->vhost_ = ruc->req_->host_;
    ruc->req_->app_ = app.empty() ? "live" : app;
    ruc->req_->stream_ = stream.empty() ? "livestream" : stream;
    ruc->req_->param_ = r->query();

    ruc->req_->ice_ufrag_ = r->query_get("ice-ufrag");
    ruc->req_->ice_pwd_ = r->query_get("ice-pwd");
    if (!ruc->req_->ice_ufrag_.empty() && (ruc->req_->ice_ufrag_.length() < SRS_ICE_UFRAG_MIN || ruc->req_->ice_ufrag_.length() > SRS_ICE_UFRAG_MAX)) {
        return srs_error_new(ERROR_RTC_INVALID_ICE, "Invalid ice-ufrag %s", ruc->req_->ice_ufrag_.c_str());
    }
    if (!ruc->req_->ice_pwd_.empty() && (ruc->req_->ice_pwd_.length() < SRS_ICE_PWD_MIN || ruc->req_->ice_pwd_.length() > SRS_ICE_PWD_MAX)) {
        return srs_error_new(ERROR_RTC_INVALID_ICE, "Invalid ice-pwd %s", ruc->req_->ice_pwd_.c_str());
    }

    // discovery vhost, resolve the vhost from config
    SrsConfDirective *parsed_vhost = config_->get_vhost(ruc->req_->vhost_);
    if (parsed_vhost) {
        ruc->req_->vhost_ = parsed_vhost->arg0();
    }

    // For client to specifies whether encrypt by SRTP.
    string srtp = r->query_get("encrypt");
    string dtls = r->query_get("dtls");

    srs_trace("RTC whip %s %s, clientip=%s, app=%s, stream=%s, offer=%dB, eip=%s, codec=%s, srtp=%s, dtls=%s, ufrag=%s, pwd=%s, param=%s",
              action.c_str(), ruc->req_->get_stream_url().c_str(), clientip.c_str(), ruc->req_->app_.c_str(), ruc->req_->stream_.c_str(),
              remote_sdp_str.length(), eip.c_str(), codec.c_str(), srtp.c_str(), dtls.c_str(), ruc->req_->ice_ufrag_.c_str(),
              ruc->req_->ice_pwd_.c_str(), ruc->req_->param_.c_str());

    ruc->eip_ = eip;
    ruc->codec_ = codec;
    ruc->publish_ = (action == "publish");

    // For client to specifies whether encrypt by SRTP.
    ruc->dtls_ = (dtls != "false");
    if (srtp.empty()) {
        ruc->srtp_ = config_->get_rtc_server_encrypt();
    } else {
        ruc->srtp_ = (srtp != "false");
    }

    // TODO: FIXME: It seems remote_sdp doesn't represents the full SDP information.
    ruc->remote_sdp_str_ = remote_sdp_str;
    if ((err = ruc->remote_sdp_.parse(remote_sdp_str)) != srs_success) {
        return srs_error_wrap(err, "parse sdp failed: %s", remote_sdp_str.c_str());
    }

    err = action == "publish" ? publish_->serve_http(w, r, ruc) : play_->serve_http(w, r, ruc);
    if (err != srs_success) {
        return srs_error_wrap(err, "serve");
    }

    return err;
}

SrsGoApiRtcNACK::SrsGoApiRtcNACK(ISrsRtcApiServer *server)
{
    server_ = server;
}

SrsGoApiRtcNACK::~SrsGoApiRtcNACK()
{
}

srs_error_t SrsGoApiRtcNACK::serve_http(ISrsHttpResponseWriter *w, ISrsHttpMessage *r)
{
    srs_error_t err = srs_success;

    SrsUniquePtr<SrsJsonObject> res(SrsJsonAny::object());

    res->set("code", SrsJsonAny::integer(ERROR_SUCCESS));

    if ((err = do_serve_http(w, r, res.get())) != srs_success) {
        srs_warn("RTC: NACK err %s", srs_error_desc(err).c_str());
        res->set("code", SrsJsonAny::integer(srs_error_code(err)));
        srs_freep(err);
    }

    return srs_api_response(w, r, res->dumps());
}

srs_error_t SrsGoApiRtcNACK::do_serve_http(ISrsHttpResponseWriter *w, ISrsHttpMessage *r, SrsJsonObject *res)
{
    string username = r->query_get("username");
    string dropv = r->query_get("drop");

    SrsJsonObject *query = SrsJsonAny::object();
    res->set("query", query);

    query->set("username", SrsJsonAny::str(username.c_str()));
    query->set("drop", SrsJsonAny::str(dropv.c_str()));
    query->set("help", SrsJsonAny::str("?username=string&drop=int"));

    int drop = ::atoi(dropv.c_str());
    if (drop <= 0) {
        return srs_error_new(ERROR_RTC_INVALID_PARAMS, "invalid drop=%s/%d", dropv.c_str(), drop);
    }

    ISrsRtcConnection *session = server_->find_rtc_session_by_username(username);
    if (!session) {
        return srs_error_new(ERROR_RTC_NO_SESSION, "no session username=%s", username.c_str());
    }

    session->simulate_nack_drop(drop);

    srs_trace("RTC: NACK session username=%s, drop=%s/%d", username.c_str(), dropv.c_str(), drop);

    return srs_success;
}
