//
// Copyright (c) 2013-2025 The SRS Authors
//
// SPDX-License-Identifier: MIT
//

#include <srs_app_rtmp_conn.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
using namespace std;

#include <srs_app_config.hpp>
#include <srs_app_edge.hpp>
#include <srs_app_factory.hpp>
#include <srs_app_hls.hpp>
#include <srs_app_http_hooks.hpp>
#include <srs_app_recv_thread.hpp>
#include <srs_app_refer.hpp>
#include <srs_app_rtc_source.hpp>
#include <srs_app_rtmp_source.hpp>
#include <srs_app_security.hpp>
#include <srs_app_server.hpp>
#include <srs_app_srt_source.hpp>
#include <srs_app_st.hpp>
#include <srs_app_statistic.hpp>
#include <srs_app_stream_token.hpp>
#include <srs_kernel_pithy_print.hpp>

#include <srs_app_utility.hpp>
#include <srs_core_autofree.hpp>
#include <srs_core_performance.hpp>
#include <srs_kernel_error.hpp>
#include <srs_kernel_log.hpp>
#include <srs_kernel_utility.hpp>
#include <srs_protocol_amf0.hpp>
#include <srs_protocol_json.hpp>
#include <srs_protocol_rtmp_msg_array.hpp>
#include <srs_protocol_rtmp_stack.hpp>
#include <srs_protocol_utility.hpp>
#ifdef SRS_RTSP
#include <srs_app_rtsp_source.hpp>
#endif

// the timeout in srs_utime_t to wait encoder to republish
// if timeout, close the connection.
#define SRS_REPUBLISH_SEND_TIMEOUT (3 * SRS_UTIME_MINUTES)

ISrsRtmpTransport::ISrsRtmpTransport()
{
}

ISrsRtmpTransport::~ISrsRtmpTransport()
{
}
// if timeout, close the connection.
#define SRS_REPUBLISH_RECV_TIMEOUT (3 * SRS_UTIME_MINUTES)

// the timeout in srs_utime_t to wait client data, when client paused
// if timeout, close the connection.
#define SRS_PAUSED_SEND_TIMEOUT (3 * SRS_UTIME_MINUTES)
// if timeout, close the connection.
#define SRS_PAUSED_RECV_TIMEOUT (3 * SRS_UTIME_MINUTES)

// when edge timeout, retry next.
#define SRS_EDGE_TOKEN_TRAVERSE_TIMEOUT (3 * SRS_UTIME_SECONDS)

SrsSimpleRtmpClient::SrsSimpleRtmpClient(string u, srs_utime_t ctm, srs_utime_t stm) : SrsBasicRtmpClient(u, ctm, stm)
{
    config_ = _srs_config;
}

SrsSimpleRtmpClient::~SrsSimpleRtmpClient()
{
    config_ = NULL;
}

// LCOV_EXCL_START
srs_error_t SrsSimpleRtmpClient::connect_app()
{
    SrsProtocolUtility utility;
    std::vector<SrsIPAddress *> &ips = utility.local_ips();
    srs_assert(config_->get_stats_network() < (int)ips.size());
    SrsIPAddress *local_ip = ips[config_->get_stats_network()];

    bool debug_srs_upnode = config_->get_debug_srs_upnode(req_->vhost_);

    return do_connect_app(local_ip->ip_, debug_srs_upnode);
}
// LCOV_EXCL_STOP

SrsClientInfo::SrsClientInfo()
{
    edge_ = false;
    req_ = new SrsRequest();
    res_ = new SrsResponse();
    type_ = SrsRtmpConnUnknown;
}

SrsClientInfo::~SrsClientInfo()
{
    srs_freep(req_);
    srs_freep(res_);
}

SrsRtmpTransport::SrsRtmpTransport(srs_netfd_t c)
{
    stfd_ = c;
    skt_ = new SrsTcpConnection(c);
}

SrsRtmpTransport::~SrsRtmpTransport()
{
    srs_freep(skt_);
}

srs_netfd_t SrsRtmpTransport::fd()
{
    return stfd_;
}

// LCOV_EXCL_START
int SrsRtmpTransport::osfd()
{
    return srs_netfd_fileno(stfd_);
}
// LCOV_EXCL_STOP

ISrsProtocolReadWriter *SrsRtmpTransport::io()
{
    return skt_;
}

srs_error_t SrsRtmpTransport::handshake()
{
    return srs_success;
}

const char *SrsRtmpTransport::transport_type()
{
    return "plaintext";
}

// LCOV_EXCL_START
srs_error_t SrsRtmpTransport::set_socket_buffer(srs_utime_t buffer_v)
{
    return skt_->set_socket_buffer(buffer_v);
}

srs_error_t SrsRtmpTransport::set_tcp_nodelay(bool v)
{
    return skt_->set_tcp_nodelay(v);
}
// LCOV_EXCL_STOP

int64_t SrsRtmpTransport::get_recv_bytes()
{
    return skt_->get_recv_bytes();
}

int64_t SrsRtmpTransport::get_send_bytes()
{
    return skt_->get_send_bytes();
}

// LCOV_EXCL_START
SrsRtmpsTransport::SrsRtmpsTransport(srs_netfd_t c) : SrsRtmpTransport(c)
{
    ssl_ = new SrsSslConnection(skt_);

    config_ = _srs_config;
}

SrsRtmpsTransport::~SrsRtmpsTransport()
{
    srs_freep(ssl_);

    config_ = NULL;
}

ISrsProtocolReadWriter *SrsRtmpsTransport::io()
{
    return ssl_;
}

srs_error_t SrsRtmpsTransport::handshake()
{
    string crt_file = config_->get_rtmps_ssl_cert();
    string key_file = config_->get_rtmps_ssl_key();
    srs_error_t err = ssl_->handshake(key_file, crt_file);
    if (err != srs_success) {
        return srs_error_wrap(err, "ssl handshake");
    }

    return srs_success;
}

const char *SrsRtmpsTransport::transport_type()
{
    return "ssl";
}
// LCOV_EXCL_STOP

SrsRtmpConn::SrsRtmpConn(ISrsRtmpTransport *transport, string cip, int cport)
{
    // Create a identify for this client.
    _srs_context->set_id(_srs_context->generate_id());

    transport_ = transport;
    ip_ = cip;
    port_ = cport;
    create_time_ = srsu2ms(srs_time_now_cached());

    trd_ = new SrsSTCoroutine("rtmp", this, _srs_context->get_id());

    kbps_ = new SrsNetworkKbps();
    kbps_->set_io(transport_->io(), transport_->io());
    delta_ = new SrsNetworkDelta();
    delta_->set_io(transport_->io(), transport_->io());

    rtmp_ = new SrsRtmpServer(transport_->io());
    refer_ = new SrsRefer();
    security_ = new SrsSecurity();
    duration_ = 0;
    wakable_ = NULL;

    mw_sleep_ = SRS_PERF_MW_SLEEP;
    mw_msgs_ = 0;
    realtime_ = SRS_PERF_MIN_LATENCY_ENABLED;
    send_min_interval_ = 0;
    tcp_nodelay_ = false;
    info_ = new SrsClientInfo();

    publish_1stpkt_timeout_ = 0;
    publish_normal_timeout_ = 0;

    app_factory_ = _srs_app_factory;
    config_ = _srs_config;
    manager_ = _srs_conn_manager;
    stream_publish_tokens_ = _srs_stream_publish_tokens;
    live_sources_ = _srs_sources;
    stat_ = _srs_stat;
    hooks_ = _srs_hooks;
    rtc_sources_ = _srs_rtc_sources;
    srt_sources_ = _srs_srt_sources;
#ifdef SRS_RTSP
    rtsp_sources_ = _srs_rtsp_sources;
#endif
}

void SrsRtmpConn::assemble()
{
    config_->subscribe(this);
}

SrsRtmpConn::~SrsRtmpConn()
{
    if (config_) {
        config_->unsubscribe(this);
    }

    trd_->interrupt();
    // wakeup the handler which need to notice.
    if (wakable_) {
        wakable_->wakeup();
    }
    srs_freep(trd_);

    srs_freep(kbps_);
    srs_freep(delta_);
    srs_freep(transport_);

    srs_freep(info_);
    srs_freep(rtmp_);
    srs_freep(refer_);
    srs_freep(security_);

    app_factory_ = NULL;
    config_ = NULL;
    manager_ = NULL;
    stream_publish_tokens_ = NULL;
    live_sources_ = NULL;
    stat_ = NULL;
    hooks_ = NULL;
    rtc_sources_ = NULL;
    srt_sources_ = NULL;
#ifdef SRS_RTSP
    rtsp_sources_ = NULL;
#endif
}

// LCOV_EXCL_START
std::string SrsRtmpConn::desc()
{
    return "RtmpConn";
}
// LCOV_EXCL_STOP

std::string srs_ipv4_string(uint32_t rip)
{
    return srs_fmt_sprintf("%d.%d.%d.%d", uint8_t(rip >> 24), uint8_t(rip >> 16), uint8_t(rip >> 8), uint8_t(rip));
}

// TODO: return detail message when error for client.
srs_error_t SrsRtmpConn::do_cycle()
{
    srs_error_t err = srs_success;

    srs_trace("RTMP client transport=%s, ip=%s:%d, fd=%d", transport_->transport_type(), ip_.c_str(), port_, transport_->osfd());

    if ((err = transport_->handshake()) != srs_success) {
        return srs_error_wrap(err, "transport handshake");
    }

    rtmp_->set_recv_timeout(SRS_CONSTS_RTMP_TIMEOUT);
    rtmp_->set_send_timeout(SRS_CONSTS_RTMP_TIMEOUT);

    if ((err = rtmp_->handshake()) != srs_success) {
        return srs_error_wrap(err, "rtmp handshake");
    }

    uint32_t rip = rtmp_->proxy_real_ip();
    std::string rips = srs_ipv4_string(rip);
    if (rip > 0) {
        srs_trace("RTMP proxy real client ip=%s", rips.c_str());
    }

    ISrsRequest *req = info_->req_;
    if ((err = rtmp_->connect_app(req)) != srs_success) {
        return srs_error_wrap(err, "rtmp connect tcUrl");
    }

    // set client ip to request.
    req->ip_ = ip_;

    srs_trace("connect app, tcUrl=%s, pageUrl=%s, swfUrl=%s, schema=%s, vhost=%s, port=%d, app=%s, args=%s",
              req->tcUrl_.c_str(), req->pageUrl_.c_str(), req->swfUrl_.c_str(),
              req->schema_.c_str(), req->vhost_.c_str(), req->port_,
              req->app_.c_str(), (req->args_ ? "(obj)" : "null"));

    // show client identity
    if (req->args_) {
        // LCOV_EXCL_START
        std::string srs_version;
        std::string srs_server_ip;
        int srs_pid = 0;
        int srs_id = 0;

        SrsAmf0Any *prop = NULL;
        if ((prop = req->args_->ensure_property_string("srs_version")) != NULL) {
            srs_version = prop->to_str();
        }
        if ((prop = req->args_->ensure_property_string("srs_server_ip")) != NULL) {
            srs_server_ip = prop->to_str();
        }
        if ((prop = req->args_->ensure_property_number("srs_pid")) != NULL) {
            srs_pid = (int)prop->to_number();
        }
        if ((prop = req->args_->ensure_property_number("srs_id")) != NULL) {
            srs_id = (int)prop->to_number();
        }

        if (srs_pid > 0) {
            srs_trace("edge-srs ip=%s, version=%s, pid=%d, id=%d",
                      srs_server_ip.c_str(), srs_version.c_str(), srs_pid, srs_id);
        }
        // LCOV_EXCL_STOP
    }

    if ((err = service_cycle()) != srs_success) {
        err = srs_error_wrap(err, "service cycle");
    }

    srs_error_t r0 = srs_success;
    if ((r0 = on_disconnect()) != srs_success) {
        err = srs_error_wrap(err, "on disconnect %s", srs_error_desc(r0).c_str());
        srs_freep(r0);
    }

    // If client is redirect to other servers, we already logged the event.
    if (srs_error_code(err) == ERROR_CONTROL_REDIRECT) {
        srs_freep(err);
    }

    return err;
}

// LCOV_EXCL_START
ISrsKbpsDelta *SrsRtmpConn::delta()
{
    return delta_;
}
// LCOV_EXCL_STOP

srs_error_t SrsRtmpConn::service_cycle()
{
    srs_error_t err = srs_success;

    ISrsRequest *req = info_->req_;

    int out_ack_size = config_->get_out_ack_size(req->vhost_);
    if (out_ack_size && (err = rtmp_->set_window_ack_size(out_ack_size)) != srs_success) {
        return srs_error_wrap(err, "rtmp: set out window ack size");
    }

    int in_ack_size = config_->get_in_ack_size(req->vhost_);
    if (in_ack_size && (err = rtmp_->set_in_window_ack_size(in_ack_size)) != srs_success) {
        return srs_error_wrap(err, "rtmp: set in window ack size");
    }

    if ((err = rtmp_->set_peer_bandwidth((int)(2.5 * 1000 * 1000), SrsPeerBandwidthDynamic)) != srs_success) {
        return srs_error_wrap(err, "rtmp: set peer bandwidth");
    }

    // get the ip which client connected.
    std::string local_ip = srs_get_local_ip(transport_->osfd());

    // set chunk size to larger.
    // set the chunk size before any larger response greater than 128,
    // to make OBS happy, @see https://github.com/ossrs/srs/issues/454
    int chunk_size = config_->get_chunk_size(req->vhost_);
    if ((err = rtmp_->set_chunk_size(chunk_size)) != srs_success) {
        return srs_error_wrap(err, "rtmp: set chunk size %d", chunk_size);
    }

    // response the client connect ok.
    if ((err = rtmp_->response_connect_app(req, local_ip.c_str())) != srs_success) {
        return srs_error_wrap(err, "rtmp: response connect app");
    }

    if ((err = rtmp_->on_bw_done()) != srs_success) {
        return srs_error_wrap(err, "rtmp: on bw down");
    }

    while (true) {
        if ((err = trd_->pull()) != srs_success) {
            return srs_error_wrap(err, "rtmp: thread quit");
        }

        err = stream_service_cycle();

        // stream service must terminated with error, never success.
        // when terminated with success, it's user required to stop.
        // TODO: FIXME: Support RTMP client timeout, https://github.com/ossrs/srs/issues/1134
        if (err == srs_success) {
            continue;
        }

        // when not system control error, fatal error, return.
        if (!srs_is_system_control_error(err)) {
            return srs_error_wrap(err, "rtmp: stream service");
        }

        // LCOV_EXCL_START
        // for republish, continue service
        if (srs_error_code(err) == ERROR_CONTROL_REPUBLISH) {
            // set timeout to a larger value, wait for encoder to republish.
            rtmp_->set_send_timeout(SRS_REPUBLISH_RECV_TIMEOUT);
            rtmp_->set_recv_timeout(SRS_REPUBLISH_SEND_TIMEOUT);

            srs_info("rtmp: retry for republish");
            srs_freep(err);
            continue;
        }

        // for "some" system control error,
        // logical accept and retry stream service.
        if (srs_error_code(err) == ERROR_CONTROL_RTMP_CLOSE) {
            // TODO: FIXME: use ping message to anti-death of socket.
            // set timeout to a larger value, for user paused.
            rtmp_->set_recv_timeout(SRS_PAUSED_RECV_TIMEOUT);
            rtmp_->set_send_timeout(SRS_PAUSED_SEND_TIMEOUT);

            srs_trace("rtmp: retry for close");
            srs_freep(err);
            continue;
        }

        // for other system control message, fatal error.
        return srs_error_wrap(err, "rtmp: reject");
        // LCOV_EXCL_STOP
    }

    return err;
}

srs_error_t SrsRtmpConn::stream_service_cycle()
{
    srs_error_t err = srs_success;

    ISrsRequest *req = info_->req_;
    if ((err = rtmp_->identify_client(info_->res_->stream_id_, info_->type_, req->stream_, req->duration_)) != srs_success) {
        return srs_error_wrap(err, "rtmp: identify client");
    }

    srs_net_url_parse_tcurl(req->tcUrl_, req->schema_, req->host_, req->vhost_, req->app_, req->stream_, req->port_, req->param_);

    // LCOV_EXCL_START
    // guess stream name
    if (req->stream_.empty()) {
        string app = req->app_, param = req->param_;
        srs_net_url_guess_stream(req->app_, req->param_, req->stream_);
        srs_trace("Guessing by app=%s, param=%s to app=%s, param=%s, stream=%s", app.c_str(), param.c_str(), req->app_.c_str(), req->param_.c_str(), req->stream_.c_str());
    }
    // LCOV_EXCL_STOP

    req->strip();
    srs_trace("client identified, type=%s, vhost=%s, app=%s, stream=%s, param=%s, duration=%dms",
              srs_client_type_string(info_->type_).c_str(), req->vhost_.c_str(), req->app_.c_str(), req->stream_.c_str(), req->param_.c_str(), srsu2msi(req->duration_));

    // discovery vhost, resolve the vhost from config
    SrsConfDirective *parsed_vhost = config_->get_vhost(req->vhost_);
    if (parsed_vhost) {
        req->vhost_ = parsed_vhost->arg0();
    }

    if (req->schema_.empty() || req->vhost_.empty() || req->port_ == 0 || req->app_.empty()) {
        return srs_error_new(ERROR_RTMP_REQ_TCURL, "discovery tcUrl failed, tcUrl=%s, schema=%s, vhost=%s, port=%d, app=%s",
                             req->tcUrl_.c_str(), req->schema_.c_str(), req->vhost_.c_str(), req->port_, req->app_.c_str());
    }

    // check vhost, allow default vhost.
    if ((err = check_vhost(true)) != srs_success) {
        return srs_error_wrap(err, "check vhost");
    }

    srs_trace("connected stream, tcUrl=%s, pageUrl=%s, swfUrl=%s, schema=%s, vhost=%s, port=%d, app=%s, stream=%s, param=%s, args=%s",
              req->tcUrl_.c_str(), req->pageUrl_.c_str(), req->swfUrl_.c_str(), req->schema_.c_str(), req->vhost_.c_str(), req->port_,
              req->app_.c_str(), req->stream_.c_str(), req->param_.c_str(), (req->args_ ? "(obj)" : "null"));

    // do token traverse before serve it.
    // @see https://github.com/ossrs/srs/pull/239
    if (true) {
        info_->edge_ = config_->get_vhost_is_edge(req->vhost_);
        bool edge_traverse = config_->get_vhost_edge_token_traverse(req->vhost_);

        // LCOV_EXCL_START
        if (info_->edge_ && edge_traverse) {
            if ((err = check_edge_token_traverse_auth()) != srs_success) {
                return srs_error_wrap(err, "rtmp: check token traverse");
            }
        }
        // LCOV_EXCL_STOP
    }

    // security check
    if ((err = security_->check(info_->type_, ip_, req)) != srs_success) {
        return srs_error_wrap(err, "rtmp: security check");
    }

    // Never allow the empty stream name, for HLS may write to a file with empty name.
    // @see https://github.com/ossrs/srs/issues/834
    if (req->stream_.empty()) {
        return srs_error_new(ERROR_RTMP_STREAM_NAME_EMPTY, "rtmp: empty stream");
    }

    // client is identified, set the timeout to service timeout.
    rtmp_->set_recv_timeout(SRS_CONSTS_RTMP_TIMEOUT);
    rtmp_->set_send_timeout(SRS_CONSTS_RTMP_TIMEOUT);

    // Acquire stream publish token to prevent race conditions across all protocols.
    SrsStreamPublishToken *publish_token_raw = NULL;
    if (info_->type_ != SrsRtmpConnPlay && (err = stream_publish_tokens_->acquire_token(req, publish_token_raw)) != srs_success) {
        return srs_error_wrap(err, "acquire stream publish token");
    }
    SrsUniquePtr<SrsStreamPublishToken> publish_token(publish_token_raw);
    if (publish_token.get()) {
        srs_trace("stream publish token acquired, type=%s, url=%s",
                  srs_client_type_string(info_->type_).c_str(), req->get_stream_url().c_str());
    }

    // find a source to serve.
    SrsSharedPtr<SrsLiveSource> live_source;
    if ((err = live_sources_->fetch_or_create(req, live_source)) != srs_success) {
        return srs_error_wrap(err, "rtmp: fetch source");
    }
    srs_assert(live_source.get() != NULL);

    bool enabled_cache = config_->get_gop_cache(req->vhost_);
    int gcmf = config_->get_gop_cache_max_frames(req->vhost_);
    srs_trace("source url=%s, ip=%s, cache=%d/%d, is_edge=%d, source_id=%s/%s",
              req->get_stream_url().c_str(), ip_.c_str(), enabled_cache, gcmf, info_->edge_, live_source->source_id().c_str(),
              live_source->pre_source_id().c_str());
    live_source->set_cache(enabled_cache);
    live_source->set_gop_cache_max_frames(gcmf);

    switch (info_->type_) {
    case SrsRtmpConnPlay: {
        // response connection start play
        if ((err = rtmp_->start_play(info_->res_->stream_id_)) != srs_success) {
            return srs_error_wrap(err, "rtmp: start play");
        }

        // We must do stat the client before hooks, because hooks depends on it.
        if ((err = stat_->on_client(_srs_context->get_id().c_str(), req, this, info_->type_)) != srs_success) {
            return srs_error_wrap(err, "rtmp: stat client");
        }

        // We must do hook after stat, because depends on it.
        if ((err = http_hooks_on_play()) != srs_success) {
            return srs_error_wrap(err, "rtmp: callback on play");
        }

        err = playing(live_source);
        http_hooks_on_stop();

        return err;
    }
    case SrsRtmpConnFMLEPublish: {
        if ((err = rtmp_->start_fmle_publish(info_->res_->stream_id_)) != srs_success) {
            return srs_error_wrap(err, "rtmp: start FMLE publish");
        }

        return publishing(live_source);
    }
    // LCOV_EXCL_START
    case SrsRtmpConnHaivisionPublish: {
        if ((err = rtmp_->start_haivision_publish(info_->res_->stream_id_)) != srs_success) {
            return srs_error_wrap(err, "rtmp: start HAIVISION publish");
        }

        return publishing(live_source);
    }
    case SrsRtmpConnFlashPublish: {
        if ((err = rtmp_->start_flash_publish(info_->res_->stream_id_)) != srs_success) {
            return srs_error_wrap(err, "rtmp: start FLASH publish");
        }

        return publishing(live_source);
    }
    default: {
        return srs_error_new(ERROR_SYSTEM_CLIENT_INVALID, "rtmp: unknown client type=%d", info_->type_);
    }}
    // LCOV_EXCL_STOP

    return err;
}

srs_error_t SrsRtmpConn::check_vhost(bool try_default_vhost)
{
    srs_error_t err = srs_success;

    ISrsRequest *req = info_->req_;
    srs_assert(req != NULL);

    SrsConfDirective *vhost = config_->get_vhost(req->vhost_, try_default_vhost);
    if (vhost == NULL) {
        return srs_error_new(ERROR_RTMP_VHOST_NOT_FOUND, "rtmp: no vhost %s", req->vhost_.c_str());
    }

    if (!config_->get_vhost_enabled(req->vhost_)) {
        return srs_error_new(ERROR_RTMP_VHOST_NOT_FOUND, "rtmp: vhost %s disabled", req->vhost_.c_str());
    }

    if (req->vhost_ != vhost->arg0()) {
        srs_trace("vhost change from %s to %s", req->vhost_.c_str(), vhost->arg0().c_str());
        req->vhost_ = vhost->arg0();
    }

    if (config_->get_refer_enabled(req->vhost_)) {
        if ((err = refer_->check(req->pageUrl_, config_->get_refer_all(req->vhost_))) != srs_success) {
            return srs_error_wrap(err, "rtmp: referer check");
        }
    }

    if ((err = http_hooks_on_connect()) != srs_success) {
        return srs_error_wrap(err, "rtmp: callback on connect");
    }

    return err;
}

srs_error_t SrsRtmpConn::playing(SrsSharedPtr<SrsLiveSource> source)
{
    srs_error_t err = srs_success;

    // Check whether thread is quiting.
    if ((err = trd_->pull()) != srs_success) {
        return srs_error_wrap(err, "thread");
    }

    // Check page referer of player.
    ISrsRequest *req = info_->req_;
    if (config_->get_refer_enabled(req->vhost_)) {
        if ((err = refer_->check(req->pageUrl_, config_->get_refer_play(req->vhost_))) != srs_success) {
            return srs_error_wrap(err, "rtmp: referer check");
        }
    }

    // When origin cluster enabled, try to redirect to the origin which is active.
    // A active origin is a server which is delivering stream.
    if (!info_->edge_ && config_->get_vhost_origin_cluster(req->vhost_) && source->inactive()) {
        return redirect_to_origin_cluster(source);
    }

    // Set the socket options for transport.
    set_sock_options();

    // Create a consumer of source.
    SrsLiveConsumer *consumer_raw = NULL;
    if ((err = source->create_consumer(consumer_raw)) != srs_success) {
        return srs_error_wrap(err, "rtmp: create consumer");
    }
    SrsUniquePtr<SrsLiveConsumer> consumer(consumer_raw);

    if ((err = source->consumer_dumps(consumer.get())) != srs_success) {
        return srs_error_wrap(err, "rtmp: dumps consumer");
    }

    // Use receiving thread to receive packets from peer.
    SrsQueueRecvThread trd(consumer.get(), rtmp_, SRS_PERF_MW_SLEEP, _srs_context->get_id());

    if ((err = trd.start()) != srs_success) {
        return srs_error_wrap(err, "rtmp: start receive thread");
    }

    // Deliver packets to peer.
    wakable_ = consumer.get();
    err = do_playing(source, consumer.get(), &trd);
    wakable_ = NULL;

    trd.stop();

    // Drop all packets in receiving thread.
    if (!trd.empty()) {
        srs_warn("drop the received %d messages", trd.size());
    }

    return err;
}

// LCOV_EXCL_START
srs_error_t SrsRtmpConn::redirect_to_origin_cluster(SrsSharedPtr<SrsLiveSource> source)
{
    srs_error_t err = srs_success;

    ISrsRequest *req = info_->req_;

    vector<string> coworkers = config_->get_vhost_coworkers(req->vhost_);
    for (int i = 0; i < (int)coworkers.size(); i++) {
        // TODO: FIXME: User may config the server itself as coworker, we must identify and ignore it.
        string host;
        int port = 0;
        string coworker = coworkers.at(i);

        string url = "http://" + coworker + "/api/v1/clusters?" + "vhost=" + req->vhost_ + "&ip=" + req->host_ + "&app=" + req->app_ + "&stream=" + req->stream_ + "&coworker=" + coworker;
        if ((err = hooks_->discover_co_workers(url, host, port)) != srs_success) {
            // If failed to discovery stream in this coworker, we should request the next one util the last.
            // @see https://github.com/ossrs/srs/issues/1223
            if (i < (int)coworkers.size() - 1) {
                continue;
            }
            return srs_error_wrap(err, "discover coworkers, url=%s", url.c_str());
        }

        string rurl = srs_net_url_encode_rtmp_url(host, port, req->host_, req->vhost_, req->app_, req->stream_, req->param_);
        srs_trace("rtmp: redirect in cluster, from=%s:%d, target=%s:%d, url=%s, rurl=%s",
                    req->host_.c_str(), req->port_, host.c_str(), port, url.c_str(), rurl.c_str());

        // Ignore if host or port is invalid.
        if (host.empty() || port == 0) {
            continue;
        }

        bool accepted = false;
        if ((err = rtmp_->redirect(req, rurl, accepted)) != srs_success) {
            srs_freep(err);
        } else {
            return srs_error_new(ERROR_CONTROL_REDIRECT, "redirected");
        }
    }

    return srs_error_new(ERROR_OCLUSTER_REDIRECT, "no origin");
}
// LCOV_EXCL_STOP

srs_error_t SrsRtmpConn::do_playing(SrsSharedPtr<SrsLiveSource> source, SrsLiveConsumer *consumer, SrsQueueRecvThread *rtrd)
{
    srs_error_t err = srs_success;

    ISrsRequest *req = info_->req_;
    srs_assert(req);
    srs_assert(consumer);

    // initialize other components
    SrsUniquePtr<SrsPithyPrint> pprint(SrsPithyPrint::create_rtmp_play());

    SrsMessageArray msgs(SRS_PERF_MW_MSGS);
    bool user_specified_duration_to_stop = (req->duration_ > 0);
    int64_t starttime = -1;

    // setup the realtime.
    realtime_ = config_->get_realtime_enabled(req->vhost_, false);
    // setup the mw config.
    // when mw_sleep changed, resize the socket send buffer.
    mw_msgs_ = config_->get_mw_msgs(req->vhost_, realtime_, false);
    mw_sleep_ = config_->get_mw_sleep(req->vhost_);
    transport_->set_socket_buffer(mw_sleep_);
    // initialize the send_min_interval
    send_min_interval_ = config_->get_send_min_interval(req->vhost_);

    srs_trace("start play smi=%dms, mw_sleep=%d, mw_msgs=%d, realtime=%d, tcp_nodelay=%d",
              srsu2msi(send_min_interval_), srsu2msi(mw_sleep_), mw_msgs_, realtime_, tcp_nodelay_);

    while (true) {
        // when source is set to expired, disconnect it.
        if ((err = trd_->pull()) != srs_success) {
            return srs_error_wrap(err, "rtmp: thread quit");
        }

        // collect elapse for pithy print.
        pprint->elapse();

        // to use isolate thread to recv, can improve about 33% performance.
        while (!rtrd->empty()) {
            SrsRtmpCommonMessage *msg = rtrd->pump();
            if ((err = process_play_control_msg(consumer, msg)) != srs_success) {
                return srs_error_wrap(err, "rtmp: play control message");
            }
        }

#ifdef SRS_PERF_QUEUE_COND_WAIT
        // wait for message to incoming.
        // @see https://github.com/ossrs/srs/issues/257
        consumer->wait(mw_msgs_, mw_sleep_);
#endif

        // Quit when recv thread error. Check recv thread error when wakeup, in order
        // to detect the client disconnecting event.
        if ((err = rtrd->error_code()) != srs_success) {
            return srs_error_wrap(err, "rtmp: recv thread");
        }

        // get messages from consumer.
        // each msg in msgs.msgs must be free, for the SrsMessageArray never free them.
        // @remark when enable send_min_interval, only fetch one message a time.
        int count = (send_min_interval_ > 0) ? 1 : 0;
        if ((err = consumer->dump_packets(&msgs, count)) != srs_success) {
            return srs_error_wrap(err, "rtmp: consumer dump packets");
        }

        // reportable
        if (pprint->can_print()) {
            kbps_->sample();
            srs_trace("-> " SRS_CONSTS_LOG_PLAY " time=%d, msgs=%d, okbps=%d,%d,%d, ikbps=%d,%d,%d, mw=%d/%d",
                      (int)pprint->age(), count, kbps_->get_send_kbps(), kbps_->get_send_kbps_30s(), kbps_->get_send_kbps_5m(),
                      kbps_->get_recv_kbps(), kbps_->get_recv_kbps_30s(), kbps_->get_recv_kbps_5m(), srsu2msi(mw_sleep_), mw_msgs_);
        }

        if (count <= 0) {
#ifndef SRS_PERF_QUEUE_COND_WAIT
            srs_usleep(mw_sleep);
#endif
            // ignore when nothing got.
            continue;
        }

        // LCOV_EXCL_START
        // only when user specifies the duration,
        // we start to collect the durations for each message.
        if (user_specified_duration_to_stop) {
            for (int i = 0; i < count; i++) {
                SrsMediaPacket *msg = msgs.msgs_[i];

                // foreach msg, collect the duration.
                // @remark: never use msg when sent it, for the protocol sdk will free it.
                if (starttime < 0 || starttime > msg->timestamp_) {
                    starttime = msg->timestamp_;
                }
                duration_ += (msg->timestamp_ - starttime) * SRS_UTIME_MILLISECONDS;
                starttime = msg->timestamp_;
            }
        }

        // sendout messages, all messages are freed by send_and_free_messages().
        // no need to assert msg, for the rtmp will assert it.
        if (count > 0 && (err = rtmp_->send_and_free_messages(msgs.msgs_, count, info_->res_->stream_id_)) != srs_success) {
            return srs_error_wrap(err, "rtmp: send %d messages", count);
        }
        // LCOV_EXCL_STOP

        // if duration specified, and exceed it, stop play live.
        // @see: https://github.com/ossrs/srs/issues/45
        if (user_specified_duration_to_stop) {
            if (duration_ >= req->duration_) {
                return srs_error_new(ERROR_RTMP_DURATION_EXCEED, "rtmp: time %d up %d", srsu2msi(duration_), srsu2msi(req->duration_));
            }
        }

        // apply the minimal interval for delivery stream in srs_utime_t.
        if (send_min_interval_ > 0) {
            srs_usleep(send_min_interval_);
        }

        // Yield to another coroutines.
        // @see https://github.com/ossrs/srs/issues/2194#issuecomment-777437476
        srs_thread_yield();
    }

    return err;
}

srs_error_t SrsRtmpConn::publishing(SrsSharedPtr<SrsLiveSource> source)
{
    srs_error_t err = srs_success;

    // Check whether thread is quiting.
    if ((err = trd_->pull()) != srs_success) {
        return srs_error_wrap(err, "thread");
    }

    ISrsRequest *req = info_->req_;

    if (config_->get_refer_enabled(req->vhost_)) {
        if ((err = refer_->check(req->pageUrl_, config_->get_refer_publish(req->vhost_))) != srs_success) {
            return srs_error_wrap(err, "rtmp: referer check");
        }
    }

    // We must do stat the client before hooks, because hooks depends on it.
    if ((err = stat_->on_client(_srs_context->get_id().c_str(), req, this, info_->type_)) != srs_success) {
        return srs_error_wrap(err, "rtmp: stat client");
    }

    // We must do hook after stat, because depends on it.
    if ((err = http_hooks_on_publish()) != srs_success) {
        return srs_error_wrap(err, "rtmp: callback on publish");
    }

    // TODO: FIXME: Should refine the state of publishing.
    srs_error_t acquire_err = acquire_publish(source);
    if ((err = acquire_err) == srs_success) {
        // use isolate thread to recv,
        // @see: https://github.com/ossrs/srs/issues/237
        SrsPublishRecvThread rtrd(rtmp_, req, transport_->osfd(), 0, this, source, _srs_context->get_id());
        rtrd.assemble();

        err = do_publishing(source, &rtrd);
        rtrd.stop();
    }

    // Release and callback when acquire publishing success, if not, we should ignore, because the source
    // is not published by this session.
    if (acquire_err == srs_success) {
        release_publish(source);
        http_hooks_on_unpublish();
    }

    return err;
}

srs_error_t SrsRtmpConn::do_publishing(SrsSharedPtr<SrsLiveSource> source, SrsPublishRecvThread *rtrd)
{
    srs_error_t err = srs_success;

    ISrsRequest *req = info_->req_;
    SrsUniquePtr<SrsPithyPrint> pprint(SrsPithyPrint::create_rtmp_publish());

    // start isolate recv thread.
    // TODO: FIXME: Pass the callback here.
    if ((err = rtrd->start()) != srs_success) {
        return srs_error_wrap(err, "rtmp: receive thread");
    }

    // initialize the publish timeout.
    publish_1stpkt_timeout_ = config_->get_publish_1stpkt_timeout(req->vhost_);
    publish_normal_timeout_ = config_->get_publish_normal_timeout(req->vhost_);
    srs_utime_t publish_kickoff_for_idle = config_->get_publish_kickoff_for_idle(req->vhost_);

    // set the sock options.
    set_sock_options();

    if (true) {
        bool mr = config_->get_mr_enabled(req->vhost_);
        srs_utime_t mr_sleep = config_->get_mr_sleep(req->vhost_);
        srs_trace("start publish mr=%d/%d, p1stpt=%d, pnt=%d, tcp_nodelay=%d", mr, srsu2msi(mr_sleep), srsu2msi(publish_1stpkt_timeout_), srsu2msi(publish_normal_timeout_), tcp_nodelay_);
    }

    // Response the start publishing message, let client start to publish messages.
    if ((err = rtmp_->start_publishing(info_->res_->stream_id_)) != srs_success) {
        return srs_error_wrap(err, "start publishing");
    }

    int64_t nb_msgs = 0;
    uint64_t nb_frames = 0;
    while (true) {
        if ((err = trd_->pull()) != srs_success) {
            return srs_error_wrap(err, "rtmp: thread quit");
        }

        // Kick off the publisher when idle for a period of timeout.
        if (source->publisher_is_idle_for(publish_kickoff_for_idle)) {
            return srs_error_new(ERROR_KICKOFF_FOR_IDLE, "kicked for idle, url=%s, timeout=%ds", req->tcUrl_.c_str(), srsu2si(publish_kickoff_for_idle));
        }

        pprint->elapse();

        // cond wait for timeout.
        if (nb_msgs == 0) {
            // when not got msgs, wait for a larger timeout.
            rtrd->wait(publish_1stpkt_timeout_);
        } else {
            rtrd->wait(publish_normal_timeout_);
        }

        // Quit when recv thread error. Check recv thread error when wakeup, in order
        // to detect the client disconnecting event.
        if ((err = rtrd->error_code()) != srs_success) {
            return srs_error_wrap(err, "rtmp: receive thread");
        }

        // LCOV_EXCL_START
        // when not got any messages, timeout.
        if (rtrd->nb_msgs() <= nb_msgs) {
            return srs_error_new(ERROR_SOCKET_TIMEOUT, "rtmp: publish timeout %dms, nb_msgs=%d",
                                 nb_msgs ? srsu2msi(publish_normal_timeout_) : srsu2msi(publish_1stpkt_timeout_), (int)nb_msgs);
        }
        nb_msgs = rtrd->nb_msgs();

        // Update the stat for video fps.
        // @remark https://github.com/ossrs/srs/issues/851
        if ((err = stat_->on_video_frames(req, (int)(rtrd->nb_video_frames() - nb_frames))) != srs_success) {
            return srs_error_wrap(err, "rtmp: stat video frames");
        }
        nb_frames = rtrd->nb_video_frames();

        // reportable
        if (pprint->can_print()) {
            kbps_->sample();
            bool mr = config_->get_mr_enabled(req->vhost_);
            srs_utime_t mr_sleep = config_->get_mr_sleep(req->vhost_);
            srs_trace("<- " SRS_CONSTS_LOG_CLIENT_PUBLISH " time=%d, okbps=%d,%d,%d, ikbps=%d,%d,%d, mr=%d/%d, p1stpt=%d, pnt=%d",
                      (int)pprint->age(), kbps_->get_send_kbps(), kbps_->get_send_kbps_30s(), kbps_->get_send_kbps_5m(),
                      kbps_->get_recv_kbps(), kbps_->get_recv_kbps_30s(), kbps_->get_recv_kbps_5m(), mr, srsu2msi(mr_sleep),
                      srsu2msi(publish_1stpkt_timeout_), srsu2msi(publish_normal_timeout_));
        }
        // LCOV_EXCL_STOP
    }

    return err;
}

srs_error_t SrsRtmpConn::acquire_publish(SrsSharedPtr<SrsLiveSource> source)
{
    srs_error_t err = srs_success;

    ISrsRequest *req = info_->req_;

    // Check whether RTMP stream is busy.
    if (!source->can_publish(info_->edge_)) {
        return srs_error_new(ERROR_SYSTEM_STREAM_BUSY, "rtmp: stream %s is busy", req->get_stream_url().c_str());
    }

    // Check whether RTC stream is busy.
    SrsSharedPtr<SrsRtcSource> rtc;
    bool rtc_server_enabled = config_->get_rtc_server_enabled();
    bool rtc_enabled = config_->get_rtc_enabled(req->vhost_);
    bool edge = config_->get_vhost_is_edge(req->vhost_);

    if (rtc_enabled && edge) {
        rtc_enabled = false;
        srs_warn("disable WebRTC for edge vhost=%s", req->vhost_.c_str());
    }

    if (rtc_server_enabled && rtc_enabled && !info_->edge_) {
        if ((err = rtc_sources_->fetch_or_create(req, rtc)) != srs_success) {
            return srs_error_wrap(err, "create source");
        }

        if (!rtc->can_publish()) {
            return srs_error_new(ERROR_SYSTEM_STREAM_BUSY, "rtc stream %s busy", req->get_stream_url().c_str());
        }
    }

    // Check whether SRT stream is busy.
    bool srt_server_enabled = config_->get_srt_enabled();
    bool srt_enabled = config_->get_srt_enabled(req->vhost_);
    if (srt_server_enabled && srt_enabled && !info_->edge_) {
        SrsSharedPtr<SrsSrtSource> srt;
        if ((err = srt_sources_->fetch_or_create(req, srt)) != srs_success) {
            return srs_error_wrap(err, "create source");
        }

        if (!srt->can_publish()) {
            return srs_error_new(ERROR_SYSTEM_STREAM_BUSY, "srt stream %s busy", req->get_stream_url().c_str());
        }
    }

#ifdef SRS_RTSP
    // RTSP only support viewer, so we don't need to check it.
    SrsSharedPtr<SrsRtspSource> rtsp;
    bool rtsp_server_enabled = config_->get_rtsp_server_enabled();
    bool rtsp_enabled = config_->get_rtsp_enabled(req->vhost_);
    if (rtsp_server_enabled && rtsp_enabled && !info_->edge_) {
        if ((err = rtsp_sources_->fetch_or_create(req, rtsp)) != srs_success) {
            return srs_error_wrap(err, "create source");
        }
    }
#endif

    // Bridge to RTC streaming.
    // TODO: FIXME: Need to convert RTMP to SRT.
    SrsRtmpBridge *bridge = new SrsRtmpBridge(app_factory_);

#if defined(SRS_FFMPEG_FIT)
    bool rtmp_to_rtc = config_->get_rtc_from_rtmp(req->vhost_);
    if (rtmp_to_rtc && edge) {
        rtmp_to_rtc = false;
        srs_warn("disable RTMP to WebRTC for edge vhost=%s", req->vhost_.c_str());
    }

    if (rtc.get() && rtmp_to_rtc) {
        bridge->enable_rtmp2rtc(rtc);
    }
#endif

#ifdef SRS_RTSP
    if (rtsp.get() && config_->get_rtsp_from_rtmp(req->vhost_)) {
        bridge->enable_rtmp2rtsp(rtsp);
    }
#endif

    if (bridge->empty()) {
        srs_freep(bridge);
    } else if ((err = bridge->initialize(req)) != srs_success) {
        srs_freep(bridge);
        return srs_error_wrap(err, "bridge init");
    }

    source->set_bridge(bridge);

    // Start publisher now.
    if (info_->edge_) {
        err = source->on_edge_start_publish();
    } else {
        err = source->on_publish();
    }

    return err;
}

void SrsRtmpConn::release_publish(SrsSharedPtr<SrsLiveSource> source)
{
    // when edge, notice edge to change state.
    // when origin, notice all service to unpublish.
    if (info_->edge_) {
        source->on_edge_proxy_unpublish();
    } else {
        source->on_unpublish();
    }
}

srs_error_t SrsRtmpConn::handle_publish_message(SrsSharedPtr<SrsLiveSource> &source, SrsRtmpCommonMessage *msg)
{
    srs_error_t err = srs_success;

    // process publish event.
    if (msg->header_.is_amf0_command() || msg->header_.is_amf3_command()) {
        SrsRtmpCommand *pkt_raw = NULL;
        if ((err = rtmp_->decode_message(msg, &pkt_raw)) != srs_success) {
            return srs_error_wrap(err, "rtmp: decode message");
        }
        SrsUniquePtr<SrsRtmpCommand> pkt(pkt_raw);

        // for flash, any packet is republish.
        if (info_->type_ == SrsRtmpConnFlashPublish) {
            // flash unpublish.
            // TODO: maybe need to support republish.
            srs_trace("flash flash publish finished.");
            return srs_error_new(ERROR_CONTROL_REPUBLISH, "rtmp: republish");
        }

        // for fmle, drop others except the fmle start packet.
        if (dynamic_cast<SrsFMLEStartPacket *>(pkt.get())) {
            SrsFMLEStartPacket *unpublish = dynamic_cast<SrsFMLEStartPacket *>(pkt.get());
            if ((err = rtmp_->fmle_unpublish(info_->res_->stream_id_, unpublish->transaction_id_)) != srs_success) {
                return srs_error_wrap(err, "rtmp: republish");
            }
            return srs_error_new(ERROR_CONTROL_REPUBLISH, "rtmp: republish");
        }

        srs_trace("fmle ignore AMF0/AMF3 command message.");
        return err;
    }

    // video, audio, data message
    if ((err = process_publish_message(source, msg)) != srs_success) {
        return srs_error_wrap(err, "rtmp: consume message");
    }

    return err;
}

srs_error_t SrsRtmpConn::process_publish_message(SrsSharedPtr<SrsLiveSource> &source, SrsRtmpCommonMessage *msg)
{
    srs_error_t err = srs_success;

    // LCOV_EXCL_START
    // for edge, directly proxy message to origin.
    if (info_->edge_) {
        if ((err = source->on_edge_proxy_publish(msg)) != srs_success) {
            return srs_error_wrap(err, "rtmp: proxy publish");
        }
        return err;
    }
    // LCOV_EXCL_STOP

    // process audio packet
    if (msg->header_.is_audio()) {
        if ((err = source->on_audio(msg)) != srs_success) {
            return srs_error_wrap(err, "rtmp: consume audio");
        }
        return err;
    }
    // process video packet
    if (msg->header_.is_video()) {
        if ((err = source->on_video(msg)) != srs_success) {
            return srs_error_wrap(err, "rtmp: consume video");
        }
        return err;
    }

    // LCOV_EXCL_START
    // process aggregate packet
    if (msg->header_.is_aggregate()) {
        if ((err = source->on_aggregate(msg)) != srs_success) {
            return srs_error_wrap(err, "rtmp: consume aggregate");
        }
        return err;
    }

    // process onMetaData
    if (msg->header_.is_amf0_data() || msg->header_.is_amf3_data()) {
        SrsRtmpCommand *pkt_raw = NULL;
        if ((err = rtmp_->decode_message(msg, &pkt_raw)) != srs_success) {
            return srs_error_wrap(err, "rtmp: decode message");
        }
        SrsUniquePtr<SrsRtmpCommand> pkt(pkt_raw);

        if (dynamic_cast<SrsOnMetaDataPacket *>(pkt.get())) {
            SrsOnMetaDataPacket *metadata = dynamic_cast<SrsOnMetaDataPacket *>(pkt.get());
            if ((err = source->on_meta_data(msg, metadata)) != srs_success) {
                return srs_error_wrap(err, "rtmp: consume metadata");
            }
            return err;
        }
        return err;
    }
    // LCOV_EXCL_STOP

    return err;
}

srs_error_t SrsRtmpConn::process_play_control_msg(SrsLiveConsumer *consumer, SrsRtmpCommonMessage *msg_raw)
{
    srs_error_t err = srs_success;

    if (!msg_raw) {
        return err;
    }
    SrsUniquePtr<SrsRtmpCommonMessage> msg(msg_raw);

    if (!msg->header_.is_amf0_command() && !msg->header_.is_amf3_command()) {
        return err;
    }

    SrsRtmpCommand *pkt_raw = NULL;
    if ((err = rtmp_->decode_message(msg.get(), &pkt_raw)) != srs_success) {
        return srs_error_wrap(err, "rtmp: decode message");
    }
    SrsUniquePtr<SrsRtmpCommand> pkt(pkt_raw);

    // for jwplayer/flowplayer, which send close as pause message.
    SrsCloseStreamPacket *close = dynamic_cast<SrsCloseStreamPacket *>(pkt.get());
    if (close) {
        return srs_error_new(ERROR_CONTROL_RTMP_CLOSE, "rtmp: close stream");
    }

    // call msg,
    // support response null first,
    // TODO: FIXME: response in right way, or forward in edge mode.
    SrsCallPacket *call = dynamic_cast<SrsCallPacket *>(pkt.get());
    if (call) {
        // LCOV_EXCL_START
        // only response it when transaction id not zero,
        // for the zero means donot need response.
        if (call->transaction_id_ > 0) {
            SrsCallResPacket *res = new SrsCallResPacket(call->transaction_id_);
            res->command_object_ = SrsAmf0Any::null();
            res->response_ = SrsAmf0Any::null();
            if ((err = rtmp_->send_and_free_packet(res, 0)) != srs_success) {
                return srs_error_wrap(err, "rtmp: send packets");
            }
        }
        return err;
        // LCOV_EXCL_STOP
    }

    // pause
    SrsPausePacket *pause = dynamic_cast<SrsPausePacket *>(pkt.get());
    if (pause) {
        if ((err = rtmp_->on_play_client_pause(info_->res_->stream_id_, pause->is_pause_)) != srs_success) {
            return srs_error_wrap(err, "rtmp: pause");
        }
        if ((err = consumer->on_play_client_pause(pause->is_pause_)) != srs_success) {
            return srs_error_wrap(err, "rtmp: pause");
        }
        return err;
    }

    // other msg.
    return err;
}

// LCOV_EXCL_START
void SrsRtmpConn::set_sock_options()
{
    ISrsRequest *req = info_->req_;

    bool nvalue = config_->get_tcp_nodelay(req->vhost_);
    if (nvalue != tcp_nodelay_) {
        tcp_nodelay_ = nvalue;

        srs_error_t err = transport_->set_tcp_nodelay(tcp_nodelay_);
        if (err != srs_success) {
            srs_warn("ignore err %s", srs_error_desc(err).c_str());
            srs_freep(err);
        }
    }
}
// LCOV_EXCL_STOP

// LCOV_EXCL_START
srs_error_t SrsRtmpConn::check_edge_token_traverse_auth()
{
    srs_error_t err = srs_success;

    ISrsRequest *req = info_->req_;
    srs_assert(req);

    vector<string> args = config_->get_vhost_edge_origin(req->vhost_)->args_;
    if (args.empty()) {
        return err;
    }

    for (int i = 0; i < (int)args.size(); i++) {
        string hostport = args.at(i);

        // select the origin.
        string server;
        int port = SRS_CONSTS_RTMP_DEFAULT_PORT;
        srs_net_split_hostport(hostport, server, port);

        SrsUniquePtr<SrsTcpClient> transport(new SrsTcpClient(server, port, SRS_EDGE_TOKEN_TRAVERSE_TIMEOUT));
        if ((err = transport->connect()) != srs_success) {
            srs_warn("Illegal edge token, tcUrl=%s, %s", req->tcUrl_.c_str(), srs_error_desc(err).c_str());
            srs_freep(err);
            continue;
        }

        SrsUniquePtr<SrsRtmpClient> client(new SrsRtmpClient(transport.get()));
        return do_token_traverse_auth(client.get());
    }

    return srs_error_new(ERROR_EDGE_PORT_INVALID, "rtmp: Illegal edge token, server=%d", (int)args.size());
}

srs_error_t SrsRtmpConn::do_token_traverse_auth(SrsRtmpClient *client)
{
    srs_error_t err = srs_success;

    ISrsRequest *req = info_->req_;
    srs_assert(client);

    client->set_recv_timeout(SRS_CONSTS_RTMP_TIMEOUT);
    client->set_send_timeout(SRS_CONSTS_RTMP_TIMEOUT);

    if ((err = client->handshake()) != srs_success) {
        return srs_error_wrap(err, "rtmp: handshake");
    }

    // for token tranverse, always take the debug info(which carries token).
    SrsServerInfo si;
    if ((err = client->connect_app(req->app_, req->tcUrl_, req, true, &si)) != srs_success) {
        return srs_error_wrap(err, "rtmp: connect tcUrl");
    }

    srs_trace("edge token auth ok, tcUrl=%s", req->tcUrl_.c_str());
    return err;
}
// LCOV_EXCL_STOP

srs_error_t SrsRtmpConn::on_disconnect()
{
    srs_error_t err = srs_success;

    http_hooks_on_close();

    // TODO: FIXME: Implements it.

    return err;
}

srs_error_t SrsRtmpConn::http_hooks_on_connect()
{
    srs_error_t err = srs_success;

    ISrsRequest *req = info_->req_;

    if (!config_->get_vhost_http_hooks_enabled(req->vhost_)) {
        return err;
    }

    // the http hooks will cause context switch,
    // so we must copy all hooks for the on_connect may freed.
    // @see https://github.com/ossrs/srs/issues/475
    vector<string> hooks;

    if (true) {
        SrsConfDirective *conf = config_->get_vhost_on_connect(req->vhost_);

        if (!conf) {
            return err;
        }

        hooks = conf->args_;
    }

    for (int i = 0; i < (int)hooks.size(); i++) {
        std::string url = hooks.at(i);
        if ((err = hooks_->on_connect(url, req)) != srs_success) {
            return srs_error_wrap(err, "rtmp on_connect %s", url.c_str());
        }
    }

    return err;
}

void SrsRtmpConn::http_hooks_on_close()
{
    ISrsRequest *req = info_->req_;

    if (!config_->get_vhost_http_hooks_enabled(req->vhost_)) {
        return;
    }

    // the http hooks will cause context switch,
    // so we must copy all hooks for the on_connect may freed.
    // @see https://github.com/ossrs/srs/issues/475
    vector<string> hooks;

    if (true) {
        SrsConfDirective *conf = config_->get_vhost_on_close(req->vhost_);

        if (!conf) {
            return;
        }

        hooks = conf->args_;
    }

    for (int i = 0; i < (int)hooks.size(); i++) {
        std::string url = hooks.at(i);
        hooks_->on_close(url, req, transport_->get_send_bytes(), transport_->get_recv_bytes());
    }
}

srs_error_t SrsRtmpConn::http_hooks_on_publish()
{
    srs_error_t err = srs_success;

    ISrsRequest *req = info_->req_;

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

void SrsRtmpConn::http_hooks_on_unpublish()
{
    ISrsRequest *req = info_->req_;

    if (!config_->get_vhost_http_hooks_enabled(req->vhost_)) {
        return;
    }

    // the http hooks will cause context switch,
    // so we must copy all hooks for the on_connect may freed.
    // @see https://github.com/ossrs/srs/issues/475
    vector<string> hooks;

    if (true) {
        SrsConfDirective *conf = config_->get_vhost_on_unpublish(req->vhost_);

        if (!conf) {
            return;
        }

        hooks = conf->args_;
    }

    for (int i = 0; i < (int)hooks.size(); i++) {
        std::string url = hooks.at(i);
        hooks_->on_unpublish(url, req);
    }
}

srs_error_t SrsRtmpConn::http_hooks_on_play()
{
    srs_error_t err = srs_success;

    ISrsRequest *req = info_->req_;

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
            return srs_error_wrap(err, "rtmp on_play %s", url.c_str());
        }
    }

    return err;
}

void SrsRtmpConn::http_hooks_on_stop()
{
    ISrsRequest *req = info_->req_;

    if (!config_->get_vhost_http_hooks_enabled(req->vhost_)) {
        return;
    }

    // the http hooks will cause context switch,
    // so we must copy all hooks for the on_connect may freed.
    // @see https://github.com/ossrs/srs/issues/475
    vector<string> hooks;

    if (true) {
        SrsConfDirective *conf = config_->get_vhost_on_stop(req->vhost_);

        if (!conf) {
            return;
        }

        hooks = conf->args_;
    }

    for (int i = 0; i < (int)hooks.size(); i++) {
        std::string url = hooks.at(i);
        hooks_->on_stop(url, req);
    }

    return;
}

srs_error_t SrsRtmpConn::start()
{
    srs_error_t err = srs_success;

    if ((err = trd_->start()) != srs_success) {
        return srs_error_wrap(err, "coroutine");
    }

    return err;
}

void SrsRtmpConn::stop()
{
    trd_->interrupt();
    trd_->stop();
}

srs_error_t SrsRtmpConn::cycle()
{
    srs_error_t err = srs_success;

    // Serve the client.
    err = do_cycle();

    // Update statistic when done.
    stat_->kbps_add_delta(get_id().c_str(), delta_);
    stat_->on_disconnect(get_id().c_str(), err);

    // Notify manager to remove it.
    // Note that we create this object, so we use manager to remove it.
    manager_->remove(this);

    // LCOV_EXCL_START
    // success.
    if (err == srs_success) {
        srs_trace("client finished.");
        return err;
    }

    // It maybe success with message.
    if (srs_error_code(err) == ERROR_SUCCESS) {
        srs_trace("client finished%s.", srs_error_summary(err).c_str());
        srs_freep(err);
        return err;
    }

    // client close peer.
    // TODO: FIXME: Only reset the error when client closed it.
    if (srs_is_client_gracefully_close(err)) {
        srs_warn("client disconnect peer. ret=%d", srs_error_code(err));
    } else if (srs_is_server_gracefully_close(err)) {
        srs_warn("server disconnect. ret=%d", srs_error_code(err));
    } else {
        srs_error("serve error %s", srs_error_desc(err).c_str());
    }
    // LCOV_EXCL_STOP

    srs_freep(err);
    return srs_success;
}

string SrsRtmpConn::remote_ip()
{
    return ip_;
}

const SrsContextId &SrsRtmpConn::get_id()
{
    return trd_->cid();
}

// LCOV_EXCL_START
void SrsRtmpConn::expire()
{
    trd_->interrupt();
}
// LCOV_EXCL_STOP

