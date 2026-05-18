/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "Util/MD5.h"
#include "Util/base64.h"
#include "RtspPusher.h"
#include "RtspSession.h"
#include "Rtcp/RtcpContext.h"
#include "Common/config.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

RtspPusher::RtspPusher(const EventPoller::Ptr &poller, const RtspMediaSource::Ptr &src) : TcpClient(poller) {
    _push_src = src;
}

RtspPusher::~RtspPusher() {
    teardown();
    DebugL;
}

void RtspPusher::sendTeardown(){
    if (alive()) {
        if (!_content_base.empty()) {
            sendRtspRequest("TEARDOWN", _content_base);
        }
        shutdown(SockException(Err_shutdown, "teardown"));
    }
}

void RtspPusher::teardown() {
    sendTeardown();
    reset();
    CLEAR_ARR(_rtp_sock);
    CLEAR_ARR(_rtcp_sock);
    _nonce.clear();
    _realm.clear();
    _track_vec.clear();
    _session_id.clear();
    _content_base.clear();
    _cseq = 1;
    _publish_timer.reset();
    _beat_timer.reset();
    _rtsp_reader.reset();
    _track_vec.clear();
    _on_res_func = nullptr;
}

void RtspPusher::publish(const string &url_str) {
    RtspUrl url;
    try {
        url.parse(url_str);
    } catch (std::exception &ex) {
        onPublishResult_l(SockException(Err_other, StrPrinter << "illegal rtsp url:" << ex.what()), false);
        return;
    }

    teardown();

    if (url._user.size()) {
        (*this)[Client::kRtspUser] = url._user;
    }
    if (url._passwd.size()) {
        (*this)[Client::kRtspPwd] = url._passwd;
        (*this)[Client::kRtspPwdIsMD5] = false;
    }

    _url = url_str;
    _rtp_type = (Rtsp::eRtpType) (int) (*this)[Client::kRtpType];
    DebugL << url._url << " " << (url._user.size() ? url._user : "null") << " "
           << (url._passwd.size() ? url._passwd : "null") << " " << _rtp_type;

    weak_ptr<RtspPusher> weak_self = static_pointer_cast<RtspPusher>(shared_from_this());
    float publish_timeout_sec = (*this)[Client::kTimeoutMS].as<int>() / 1000.0f;
    _publish_timer.reset(new Timer(publish_timeout_sec, [weak_self]() {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return false;
        }
        strong_self->onPublishResult_l(SockException(Err_timeout, "publish rtsp timeout"), false);
        return false;
    }, getPoller()));

    if (!(*this)[Client::kNetAdapter].empty()) {
        setNetAdapter((*this)[Client::kNetAdapter]);
    }

    startConnect(url._host, url._port, publish_timeout_sec);
}

void RtspPusher::onPublishResult_l(const SockException &ex, bool handshake_done) {
    DebugL << ex.what();
    if (ex.getErrCode() == Err_shutdown) {
        // 主动shutdown的，不触发回调  [AUTO-TRANSLATED:bd97b1c1]
        // Actively shutdown, do not trigger callback
        return;
    }
    if (!handshake_done) {
        // 播放结果回调  [AUTO-TRANSLATED:a5714269]
        // Playback result callback
        _publish_timer.reset();
        onPublishResult(ex);
    } else {
        // 播放成功后异常断开回调  [AUTO-TRANSLATED:b5c5fa80]
        // Callback for abnormal disconnection after playback success
        onShutdown(ex);
    }

    if (ex) {
        sendTeardown();
    }
}

void RtspPusher::onError(const SockException &ex) {
    // 定时器_pPublishTimer为空后表明握手结束了  [AUTO-TRANSLATED:630ec31e]
    // The timer _pPublishTimer is empty, indicating that the handshake is over
    onPublishResult_l(ex, !_publish_timer);
}

void RtspPusher::onConnect(const SockException &err) {
    if (err) {
        onPublishResult_l(err, false);
        return;
    }
    sendAnnounce();
}

void RtspPusher::onRecv(const Buffer::Ptr &buf){
    try {
        input(buf->data(), buf->size());
    } catch (exception &e) {
        SockException ex(Err_other, e.what());
        // 定时器_pPublishTimer为空后表明握手结束了  [AUTO-TRANSLATED:630ec31e]
        // The timer _pPublishTimer is empty, indicating that the handshake is over
        onPublishResult_l(ex, !_publish_timer);
    }
}

void RtspPusher::onWholeRtspPacket(Parser &parser) {
    decltype(_on_res_func) func;
    _on_res_func.swap(func);
    if (func) {
        func(parser);
    }
    parser.clear();
}

void RtspPusher::onRtpPacket(const char *data, size_t len) {
    int trackIdx = -1;
    uint8_t interleaved = data[1];
    if (interleaved % 2 != 0) {
        trackIdx = getTrackIndexByInterleaved(interleaved - 1);
        onRtcpPacket(trackIdx, _track_vec[trackIdx], (uint8_t *) data + RtpPacket::kRtpTcpHeaderSize, len - RtpPacket::kRtpTcpHeaderSize);
    }
}

void RtspPusher::onRtcpPacket(int track_idx, SdpTrack::Ptr &track, uint8_t *data, size_t len){
    auto rtcp_arr = RtcpHeader::loadFromBytes((char *) data, len);
    for (auto &rtcp : rtcp_arr) {
        _rtcp_context[track_idx]->onRtcp(rtcp);
    }
}

void RtspPusher::sendAnnounce() {
    auto src = _push_src.lock();
    if (!src) {
        throw std::runtime_error("the media source was released");
    }
    // 解析sdp  [AUTO-TRANSLATED:a2d549e2]
    // Parse sdp
    _sdp_parser.load(src->getSdp());
    _track_vec = _sdp_parser.getAvailableTrack();
    if (_track_vec.empty()) {
        throw std::runtime_error("无有效的Sdp Track");
    }
    _rtcp_context.clear();
    for (auto &track : _track_vec) {
        _rtcp_context.emplace_back(std::make_shared<RtcpContextForSend>());
    }
    _on_res_func = std::bind(&RtspPusher::handleResAnnounce, this, placeholders::_1);
    sendRtspRequest("ANNOUNCE", _url, {}, src->getSdp());
}

void RtspPusher::handleResAnnounce(const Parser &parser) {
    string authInfo = parser["WWW-Authenticate"];
    // 发送DESCRIBE命令后的回复  [AUTO-TRANSLATED:924afd2e]
    // Reply after sending DESCRIBE command
    if ((parser.status() == "401") && handleAuthenticationFailure(authInfo)) {
        sendAnnounce();
        return;
    }
    if (parser.status() == "302") {
        auto newUrl = parser["Location"];
        if (newUrl.empty()) {
            throw std::runtime_error("未找到Location字段(跳转url)");
        }
        publish(newUrl);
        return;
    }
    if (parser.status() != "200") {
        throw std::runtime_error(StrPrinter << "ANNOUNCE:" << parser.status() << " " << parser.statusStr());
    }
    _content_base = parser["Content-Base"];

    if (_content_base.empty()) {
        _content_base = _url;
    }
    if (_content_base.back() == '/') {
        _content_base.pop_back();
    }
    
    _session_id = parser["Session"];

    sendSetup(0);
}

bool RtspPusher::handleAuthenticationFailure(const string &params_str) {
    if (!_realm.empty()) {
        // 已经认证过了  [AUTO-TRANSLATED:3c8ce1d6]
        // Already authenticated
        return false;
    }

    char *realm = new char[params_str.size()];
    char *nonce = new char[params_str.size()];
    char *stale = new char[params_str.size()];
    onceToken token(nullptr, [&]() {
        delete[] realm;
        delete[] nonce;
        delete[] stale;
    });

    if (sscanf(params_str.data(), "Digest realm=\"%[^\"]\", nonce=\"%[^\"]\", stale=%[a-zA-Z]", realm, nonce, stale) == 3) {
        _realm = (const char *) realm;
        _nonce = (const char *) nonce;
        return true;
    }
    if (sscanf(params_str.data(), "Digest realm=\"%[^\"]\", nonce=\"%[^\"]\"", realm, nonce) == 2) {
        _realm = (const char *) realm;
        _nonce = (const char *) nonce;
        return true;
    }
    if (sscanf(params_str.data(), "Basic realm=\"%[^\"]\"", realm) == 1) {
        _realm = (const char *) realm;
        return true;
    }
    return false;
}

// 有必要的情况下创建udp端口  [AUTO-TRANSLATED:b59b7389]
// Create UDP port if necessary
void RtspPusher::createUdpSockIfNecessary(int track_idx){
    auto &rtpSockRef = _rtp_sock[track_idx];
    auto &rtcpSockRef = _rtcp_sock[track_idx];
    if (!rtpSockRef || !rtcpSockRef) {
        std::pair<Socket::Ptr, Socket::Ptr> pr = std::make_pair(createSocket(), createSocket());
        makeSockPair(pr, get_local_ip());
        rtpSockRef = pr.first;
        rtcpSockRef = pr.second;
    }
}

void RtspPusher::sendSetup(unsigned int track_idx) {
    _on_res_func = std::bind(&RtspPusher::handleResSetup, this, placeholders::_1, track_idx);
    auto &track = _track_vec[track_idx];
    auto control_url = track->getControlUrl(_content_base);
    switch (_rtp_type) {
        case Rtsp::RTP_TCP: {
            sendRtspRequest("SETUP", control_url, {"Transport",
                                                   StrPrinter << "RTP/AVP/TCP;unicast;interleaved=" << track_idx * 2
                                                           << "-" << track_idx * 2 + 1 << ";mode=record"});
        }
            break;
        case Rtsp::RTP_UDP: {
            createUdpSockIfNecessary(track_idx);
            int port = _rtp_sock[track_idx]->get_local_port();
            sendRtspRequest("SETUP", control_url,
                            {"Transport", StrPrinter << "RTP/AVP;unicast;client_port=" << port << "-" << port + 1 << ";mode=record"});
        }
            break;
        default:
            break;
    }
}

void RtspPusher::handleResSetup(const Parser &parser, unsigned int track_idx) {
    if (parser.status() != "200") {
        throw std::runtime_error(StrPrinter << "SETUP:" << parser.status() << " " << parser.statusStr() << endl);
    }
    if (track_idx == 0) {
        _session_id = parser["Session"];
        _session_id.append(";");
        _session_id = findSubString(_session_id.data(), nullptr, ";");
    }

    auto transport = parser["Transport"];
    if (transport.find("TCP") != string::npos || transport.find("interleaved") != string::npos) {
        _rtp_type = Rtsp::RTP_TCP;
        string interleaved = findSubString(findSubString((transport + ";").data(), "interleaved=", ";").data(), NULL, "-");
        _track_vec[track_idx]->_interleaved = atoi(interleaved.data());
    } else if (transport.find("multicast") != string::npos) {
        throw std::runtime_error("SETUP rtsp pusher can not support multicast!");
    } else {
        _rtp_type = Rtsp::RTP_UDP;
        createUdpSockIfNecessary(track_idx);
        const char *strPos = "server_port=";
        auto port_str = findSubString((transport + ";").data(), strPos, ";");
        uint16_t rtp_port = atoi(findSubString(port_str.data(), NULL, "-").data());
        uint16_t rtcp_port = atoi(findSubString(port_str.data(), "-", NULL).data());
        auto &rtp_sock = _rtp_sock[track_idx];
        auto &rtcp_sock = _rtcp_sock[track_idx];

        auto rtpto = SockUtil::make_sockaddr(get_peer_ip().data(), rtp_port);
        // 设置rtp发送目标，为后续发送rtp做准备  [AUTO-TRANSLATED:5ae9bd72]
        // Set RTP sending target, prepare for subsequent RTP sending
        rtp_sock->bindPeerAddr((struct sockaddr *) &(rtpto));

        // 设置rtcp发送目标，为后续发送rtcp做准备  [AUTO-TRANSLATED:a487732d]
        // Set RTCP sending target, prepare for subsequent RTCP sending
        auto rtcpto = SockUtil::make_sockaddr(get_peer_ip().data(), rtcp_port);
        rtcp_sock->bindPeerAddr((struct sockaddr *)&(rtcpto));

        auto peer_ip = get_peer_ip();
        weak_ptr<RtspPusher> weakSelf = static_pointer_cast<RtspPusher>(shared_from_this());
        if(rtcp_sock) {
            // 设置rtcp over udp接收回调处理函数  [AUTO-TRANSLATED:59963785]
            // Set RTCP over UDP receive callback handler
            rtcp_sock->setOnRead([peer_ip, track_idx, weakSelf](const Buffer::Ptr &buf, struct sockaddr *addr , int addr_len) {
                auto strongSelf = weakSelf.lock();
                if (!strongSelf) {
                    return;
                }
                if (SockUtil::inet_ntoa(addr) != peer_ip) {
                    WarnL << "收到其他地址的rtcp数据:" << SockUtil::inet_ntoa(addr);
                    return;
                }
                strongSelf->onRtcpPacket(track_idx, strongSelf->_track_vec[track_idx], (uint8_t *) buf->data(), buf->size());
            });
        }
    }

    RtspSplitter::enableRecvRtp(_rtp_type == Rtsp::RTP_TCP);

    if (track_idx < _track_vec.size() - 1) {
        // 需要继续发送SETUP命令  [AUTO-TRANSLATED:fddda4c6]
        // Need to continue sending SETUP command
        sendSetup(track_idx + 1);
        return;
    }

    sendRecord();
}

void RtspPusher::sendOptions() {
    _on_res_func = [](const Parser &parser) {};
    sendRtspRequest("OPTIONS", _content_base);
}

void RtspPusher::updateRtcpContext(const RtpPacket::Ptr &rtp){
    int track_index = getTrackIndexByTrackType(rtp->type);
    auto &ticker = _rtcp_send_ticker[track_index];
    auto &rtcp_ctx = _rtcp_context[track_index];
    rtcp_ctx->onRtp(rtp->getSeq(), rtp->getStamp(), rtp->ntp_stamp, rtp->sample_rate, rtp->size() - RtpPacket::kRtpTcpHeaderSize);
    if (!rtp->ntp_stamp && !rtp->getStamp()) {
        // 忽略时间戳都为0的rtp  [AUTO-TRANSLATED:6b793565]
        // Ignore RTP with all timestamps being 0
        return;
    }
    //send rtcp every 5 second
    if (ticker.elapsedTime() > 5 * 1000) {
        ticker.resetTime();
        static auto send_rtcp = [](RtspPusher *thiz, int index, Buffer::Ptr ptr) {
            if (thiz->_rtp_type == Rtsp::RTP_TCP) {
                auto &track = thiz->_track_vec[index];
                thiz->send(makeRtpOverTcpPrefix((uint16_t) (ptr->size()), track->_interleaved + 1));
                thiz->send(std::move(ptr));
            } else {
                thiz->_rtcp_sock[index]->send(std::move(ptr));
            }
        };

        auto ssrc = rtp->getSSRC();
        auto rtcp = rtcp_ctx->createRtcpSR(ssrc + 1);
        auto rtcp_sdes = RtcpSdes::create({kServerName});
        rtcp_sdes->chunks.type = (uint8_t) SdesType::RTCP_SDES_CNAME;
        rtcp_sdes->chunks.ssrc = htonl(ssrc);
        send_rtcp(this, track_index, std::move(rtcp));
        send_rtcp(this, track_index, RtcpHeader::toBuffer(rtcp_sdes));
    }
}

void RtspPusher::sendRtpPacket(const RtspMediaSource::RingDataType &pkt) {
    switch (_rtp_type) {
        case Rtsp::RTP_TCP: {
            size_t i = 0;
            auto size = pkt->size();
            setSendFlushFlag(false);
            pkt->for_each([&](const RtpPacket::Ptr &rtp) {
                updateRtcpContext(rtp);
                if (++i == size) {
                    setSendFlushFlag(true);
                }
                send(rtp);
            });
            break;
        }

        case Rtsp::RTP_UDP: {
            size_t i = 0;
            auto size = pkt->size();
            pkt->for_each([&](const RtpPacket::Ptr &rtp) {
                updateRtcpContext(rtp);
                int iTrackIndex = getTrackIndexByTrackType(rtp->type);
                auto &pSock = _rtp_sock[iTrackIndex];
                if (!pSock) {
                    shutdown(SockException(Err_shutdown, "udp sock not opened yet"));
                    return;
                }

                pSock->send(std::make_shared<BufferRtp>(rtp, RtpPacket::kRtpTcpHeaderSize), nullptr, 0, ++i == size);
            });
            break;
        }
        default : break;
    }
}

int RtspPusher::getTrackIndexByInterleaved(int interleaved) const {
    for (size_t i = 0; i < _track_vec.size(); ++i) {
        if (_track_vec[i]->_interleaved == interleaved) {
            return i;
        }
    }
    if (_track_vec.size() == 1) {
        return 0;
    }
    throw SockException(Err_shutdown, StrPrinter << "no such track with interleaved:" << interleaved);
}

int RtspPusher::getTrackIndexByTrackType(TrackType type) const {
    for (size_t i = 0; i < _track_vec.size(); ++i) {
        if (type == _track_vec[i]->_type) {
            return i;
        }
    }
    if (_track_vec.size() == 1) {
        return 0;
    }
    throw SockException(Err_shutdown, StrPrinter << "no such track with type:" << getTrackString(type));
}

void RtspPusher::sendRecord() {
    _on_res_func = [this](const Parser &parser) {
        auto src = _push_src.lock();
        if (!src) {
            throw std::runtime_error("the media source was released");
        }

        src->pause(false);
        _rtsp_reader = src->getRing()->attach(getPoller());
        weak_ptr<RtspPusher> weak_self = static_pointer_cast<RtspPusher>(shared_from_this());
        _rtsp_reader->setReadCB([weak_self](const RtspMediaSource::RingDataType &pkt) {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }
            strong_self->sendRtpPacket(pkt);
        });
        _rtsp_reader->setDetachCB([weak_self]() {
            auto strong_self = weak_self.lock();
            if (strong_self) {
                strong_self->onPublishResult_l(SockException(Err_other, "媒体源被释放"), !strong_self->_publish_timer);
            }
        });
        if (_rtp_type != Rtsp::RTP_TCP) {
            // ///////////////////////心跳/////////////////////////////////  [AUTO-TRANSLATED:4e72777b]
            // ///////////////////////Heartbeat/////////////////////////////////
            _beat_timer.reset(new Timer((*this)[Client::kBeatIntervalMS].as<int>() / 1000.0f, [weak_self]() {
                auto strong_self = weak_self.lock();
                if (!strong_self) {
                    return false;
                }
                strong_self->sendOptions();
                return true;
            }, getPoller()));
        }
        onPublishResult_l(SockException(Err_success, "success"), false);
        // 提升发送性能  [AUTO-TRANSLATED:90630751]
        // Improve sending performance
        setSocketFlags();
    };
    sendRtspRequest("RECORD", _content_base, {"Range", "npt=0.000-"});
}

void RtspPusher::setSocketFlags(){
    GET_CONFIG(int, merge_write_ms, General::kMergeWriteMS);
    if (merge_write_ms > 0) {
        // 提高发送性能  [AUTO-TRANSLATED:de96ec30]
        // Improve sending performance
        setSendFlags(SOCKET_DEFAULE_FLAGS | FLAG_MORE);
        SockUtil::setNoDelay(getSock()->rawFD(), false);
    }
}

void RtspPusher::sendRtspRequest(const string &cmd, const string &url, const std::initializer_list<string> &header,const string &sdp ) {
    string key;
    StrCaseMap header_map;
    int i = 0;
    for (auto &val : header) {
        if (++i % 2 == 0) {
            header_map.emplace(key, val);
        } else {
            key = val;
        }
    }
    sendRtspRequest(cmd, url, header_map, sdp);
}
void RtspPusher::sendRtspRequest(const string &cmd, const string &url,const StrCaseMap &header_const,const string &sdp ) {
    auto header = header_const;
    header.emplace("CSeq", StrPrinter << _cseq++);
    header.emplace("User-Agent", kServerName);

    if (!_session_id.empty()) {
        header.emplace("Session", _session_id);
    }

    if (!_realm.empty() && !(*this)[Client::kRtspUser].empty()) {
        if (!_nonce.empty()) {
            // MD5认证  [AUTO-TRANSLATED:57936f0b]
            // MD5 authentication
            /*
            response计算方法如下：
            RTSP客户端应该使用username + password并计算response如下:
            (1)当password为MD5编码,则
                response = md5( password:nonce:md5(public_method:url)  );
            (2)当password为ANSI字符串,则
                response= md5( md5(username:realm:password):nonce:md5(public_method:url) );
             /*
             The response calculation method is as follows:
             The RTSP client should use username + password and calculate the response as follows:
             (1) When password is MD5 encoded, then
             response = md5( password:nonce:md5(public_method:url)  );
             (2) When password is ANSI string, then
             response= md5( md5(username:realm:password):nonce:md5(public_method:url) );
             
             * [AUTO-TRANSLATED:7858b67d]
             */
            string encrypted_pwd = (*this)[Client::kRtspPwd];
            if (!(*this)[Client::kRtspPwdIsMD5].as<bool>()) {
                encrypted_pwd = MD5((*this)[Client::kRtspUser] + ":" + _realm + ":" + encrypted_pwd).hexdigest();
            }
            auto response = MD5(encrypted_pwd + ":" + _nonce + ":" + MD5(cmd + ":" + url).hexdigest()).hexdigest();
            _StrPrinter printer;
            printer << "Digest ";
            printer << "username=\"" << (*this)[Client::kRtspUser] << "\", ";
            printer << "realm=\"" << _realm << "\", ";
            printer << "nonce=\"" << _nonce << "\", ";
            printer << "uri=\"" << url << "\", ";
            printer << "response=\"" << response << "\"";
            header.emplace("Authorization", printer);
        } else if (!(*this)[Client::kRtspPwdIsMD5].as<bool>()) {
            // base64认证  [AUTO-TRANSLATED:06d26447]
            // base64 authentication
            auto authStrBase64 = encodeBase64((*this)[Client::kRtspUser] + ":" + (*this)[Client::kRtspPwd]);
            header.emplace("Authorization", StrPrinter << "Basic " << authStrBase64);
        }
    }

    if (!sdp.empty()) {
        header.emplace("Content-Length", StrPrinter << sdp.size());
        header.emplace("Content-Type", "application/sdp");
    }

    _StrPrinter printer;
    printer << cmd << " " << url << " RTSP/1.0\r\n";
    for (auto &pr : header) {
        printer << pr.first << ": " << pr.second << "\r\n";
    }

    printer << "\r\n";

    if (!sdp.empty()) {
        printer << sdp;
    }
    SockSender::send(std::move(printer));
}

size_t RtspPusher::getSendSpeed() {
    size_t ret = TcpClient::getSendSpeed();
    for (auto &rtp : _rtp_sock) {
        if (rtp) {
            ret += rtp->getSendSpeed();
        }
    }
    for (auto &rtcp : _rtcp_sock) {
        if (rtcp) {
            ret += rtcp->getSendSpeed();
        }
    }

    return ret;
}

size_t RtspPusher::getSendTotalBytes() {
    size_t ret = TcpClient::getSendTotalBytes();
    for (auto &rtp : _rtp_sock) {
        if (rtp) {
            ret += rtp->getSendTotalBytes();
        }
    }
    for (auto &rtcp : _rtcp_sock) {
        if (rtcp) {
            ret += rtcp->getSendTotalBytes();
        }
    }
    return ret;
}
} /* namespace mediakit */

//////////////////////////////////////////////////////////////////////////
// RtspPusherOnvif - ONVIF Backchannel 推流器
// 使用播放器流程：DESCRIBE → SETUP → PLAY
//////////////////////////////////////////////////////////////////////////

namespace mediakit {

RtspPusherOnvif::RtspPusherOnvif(const EventPoller::Ptr &poller, const RtspMediaSource::Ptr &src)
    : RtspPlayer(poller) {
    _bOnvifBackchannel = true;
    _push_src = src;
}

RtspPusherOnvif::~RtspPusherOnvif() {
    teardown();
}

void RtspPusherOnvif::publish(const string &url_str) {
    _keepalive_url = url_str;
    RtspUrl url;
    try {
        url.parse(url_str);
    } catch (std::exception &ex) {
        onPublishResult(SockException(Err_other, StrPrinter << "illegal rtsp url:" << ex.what()));
        return;
    }
    if (url._user.size()) {
        (*this)[Client::kRtspUser] = url._user;
    }
    if (url._passwd.size()) {
        (*this)[Client::kRtspPwd] = url._passwd;
        (*this)[Client::kRtspPwdIsMD5] = false;
    }
    play(url_str);
}

void RtspPusherOnvif::teardown() {
    RtspPlayer::teardown();
    _rtsp_reader.reset();
    _rtcp_context.clear();
}

bool RtspPusherOnvif::onCheckSDP(const std::string &sdp) {
    // ONVIF Backchannel: 过滤掉 recvonly 的 track，只保留 sendonly 的
    // 这样会选择 trackID=5 (sendonly) 而不是 trackID=1 (recvonly)
    std::vector<SdpTrack::Ptr> sendonly_tracks;
    std::vector<SdpTrack::Ptr> audio_tracks;

    for (auto &track : _sdp_track) {
        // 跳过非音频/视频 track（如 TrackTitle）
        if (track->_type != TrackAudio && track->_type != TrackVideo) {
            continue;
        }

        // 打印所有属性用于调试
        std::string attrs_str;
        for (auto &attr : track->_attr) {
            attrs_str += attr.first + "=" + attr.second + "; ";
        }
        InfoL << "ONVIF Backchannel: track " << track->_control << " type=" << track->_type << " attrs: [" << attrs_str << "]";

        // 收集所有音频 track
        if (track->_type == TrackAudio) {
            audio_tracks.emplace_back(track);
        }

        // 检查是否有 sendonly 属性
        auto it = track->_attr.find("sendonly");
        if (it != track->_attr.end()) {
            sendonly_tracks.emplace_back(track);
            InfoL << "ONVIF Backchannel: found sendonly track: " << track->_control;
        }
    }

    if (!sendonly_tracks.empty()) {
        // 找到 sendonly track，使用它
        _sdp_track = std::move(sendonly_tracks);
    } else if (audio_tracks.size() > 1) {
        // 没有 sendonly 属性但有多个音频 track，选择最后一个（通常是 Backchannel）
        // ONVIF Backchannel 通常是 trackID=5，排在 trackID=1 (recvonly) 后面
        _sdp_track.clear();
        _sdp_track.emplace_back(audio_tracks.back());
        WarnL << "ONVIF Backchannel: no sendonly found, using last audio track: " << audio_tracks.back()->_control;
    } else {
        // 无法确定 Backchannel，使用所有 track
        WarnL << "ONVIF Backchannel: no sendonly track found, cannot determine backchannel";
    }

    _rtcp_context.clear();
    for (auto &track : _sdp_track) {
        _rtcp_context.emplace_back(std::make_shared<RtcpContextForSend>());
    }
    return true;
}

void RtspPusherOnvif::onRecvRTP(RtpPacket::Ptr rtp, const SdpTrack::Ptr &track) {
    // ONVIF Backchannel 不接收 RTP，只发送
}

void RtspPusherOnvif::onPlaySuccess() {
    auto src = _push_src.lock();
    if (!src) {
        onShutdown(SockException(Err_other, "media source released"));
        return;
    }
    // 调试日志：检查 socket 状态
    InfoL << "ONVIF Backchannel: onPlaySuccess, _rtp_type=" << _rtp_type
          << ", _rtp_sock[0]=" << (_rtp_sock[0] ? std::to_string(_rtp_sock[0]->get_local_port()) : "null")
          << ", _rtp_sock[1]=" << (_rtp_sock[1] ? std::to_string(_rtp_sock[1]->get_local_port()) : "null")
          << ", _rtcp_sock[0]=" << (_rtcp_sock[0] ? std::to_string(_rtcp_sock[0]->get_local_port()) : "null");
    src->pause(false);
    _rtsp_reader = src->getRing()->attach(getPoller());
    weak_ptr<RtspPusherOnvif> weak_self = dynamic_pointer_cast<RtspPusherOnvif>(shared_from_this());
    _rtsp_reader->setReadCB([weak_self](const RtspMediaSource::RingDataType &pkt) {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }
        strong_self->sendRtpPacket(pkt);
    });
    _rtsp_reader->setDetachCB([weak_self]() {
        auto strong_self = weak_self.lock();
        if (strong_self) {
            strong_self->onShutdown(SockException(Err_other, "media source released"));
        }
    });
}

void RtspPusherOnvif::sendRtpPacket(const RtspMediaSource::RingDataType &pkt) {
    switch (_rtp_type) {
        case Rtsp::RTP_TCP: {
            size_t i = 0;
            auto size = pkt->size();
            setSendFlushFlag(false);
            pkt->for_each([&](const RtpPacket::Ptr &rtp) {
                updateRtcpContext(rtp);
                if (++i == size) {
                    setSendFlushFlag(true);
                }
                // 使用 SETUP 响应中服务器返回的 interleaved channel
                int track_index = getTrackIndexByTrackType(rtp->type);
                auto &track = _sdp_track[track_index];
                // 发送 TCP prefix（$ + channel + length）
                send(makeRtpOverTcpPrefix((uint16_t)(rtp->size() - RtpPacket::kRtpTcpHeaderSize), track->_interleaved));
                // 发送 RTP 数据（跳过前 4 字节的旧 header）
                send(std::make_shared<BufferRtp>(rtp, RtpPacket::kRtpTcpHeaderSize));
            });
            break;
        }
        case Rtsp::RTP_UDP: {
            pkt->for_each([&](const RtpPacket::Ptr &rtp) {
                updateRtcpContext(rtp);
                int iTrackIndex = getTrackIndexByTrackType(rtp->type);
                auto &pSock = _rtp_sock[iTrackIndex];
                if (!pSock) {
                    WarnL << "ONVIF Backchannel: _rtp_sock[" << iTrackIndex << "] is null, rtp_type=" << _rtp_type;
                    shutdown(SockException(Err_shutdown, "udp sock not opened yet"));
                    return;
                }
                static bool s_logged_port = false;
                if (!s_logged_port) {
                    s_logged_port = true;
                    InfoL << "ONVIF Backchannel: sending RTP from local port " << pSock->get_local_port()
                          << " to server port, track_index=" << iTrackIndex;
                }
                pSock->send(std::make_shared<BufferRtp>(rtp, RtpPacket::kRtpTcpHeaderSize), nullptr, 0);
            });
            break;
        }
        default:
            WarnL << "ONVIF Backchannel: unknown rtp_type=" << _rtp_type;
            break;
    }
}

void RtspPusherOnvif::updateRtcpContext(const RtpPacket::Ptr &rtp) {
    int track_index = getTrackIndexByTrackType(rtp->type);
    auto &ticker = _rtcp_send_ticker[track_index];
    auto &rtcp_ctx = _rtcp_context[track_index];
    rtcp_ctx->onRtp(rtp->getSeq(), rtp->getStamp(), rtp->ntp_stamp, rtp->sample_rate, rtp->size() - RtpPacket::kRtpTcpHeaderSize);

    // 每 30 秒发送一次 RTSP OPTIONS 保活命令，防止 session 超时
    static int keepalive_interval_ms = 30 * 1000;
    if (ticker.elapsedTime() > keepalive_interval_ms) {
        ticker.resetTime();

        // 发送 RTSP OPTIONS 保活命令（直接构造并发送）
        _StrPrinter printer;
        printer << "OPTIONS " << _keepalive_url << " RTSP/1.0\r\n";
        printer << "CSeq: " << ++_keepalive_cseq << "\r\n";
        printer << "User-Agent: " << kServerName << "\r\n";
        printer << "\r\n";
        send(std::move(printer));

        // 发送 RTCP SR
        static auto send_rtcp = [](RtspPusherOnvif *thiz, int index, Buffer::Ptr ptr) {
            if (thiz->_rtp_type == Rtsp::RTP_TCP) {
                auto &track = thiz->_sdp_track[index];
                thiz->send(makeRtpOverTcpPrefix((uint16_t)(ptr->size()), track->_interleaved + 1));
                thiz->send(std::move(ptr));
            } else {
                thiz->_rtcp_sock[index]->send(std::move(ptr));
            }
        };
        auto ssrc = rtp->getSSRC();
        auto rtcp = rtcp_ctx->createRtcpSR(ssrc + 1);
        auto rtcp_sdes = RtcpSdes::create({kServerName});
        rtcp_sdes->chunks.type = (uint8_t)SdesType::RTCP_SDES_CNAME;
        rtcp_sdes->chunks.ssrc = htonl(ssrc);
        send_rtcp(this, track_index, std::move(rtcp));
        send_rtcp(this, track_index, RtcpHeader::toBuffer(rtcp_sdes));
    }
}

int RtspPusherOnvif::getTrackIndexByTrackType(TrackType type) const {
    for (size_t i = 0; i < _sdp_track.size(); ++i) {
        if (type == _sdp_track[i]->_type) {
            return i;
        }
    }
    if (_sdp_track.size() == 1) {
        return 0;
    }
    throw SockException(Err_shutdown, StrPrinter << "no such track with type:" << getTrackString(type));
}

PusherBase::Ptr createPusherOnvif(const EventPoller::Ptr &poller, const RtspMediaSource::Ptr &src) {
    auto weak_poller = std::weak_ptr<EventPoller>(poller);
    auto release_func = [weak_poller](PusherBase *ptr) {
        if (auto p = weak_poller.lock()) {
            p->async([ptr]() {
                onceToken token(nullptr, [&]() { delete ptr; });
                ptr->teardown();
            });
        } else {
            delete ptr;
        }
    };
    return PusherBase::Ptr(new RtspPusherOnvif(poller, src), release_func);
}

} /* namespace mediakit */
