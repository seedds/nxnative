#include "webrtc_streamer.h"
#include "vtb_encoder.h"
#include "opus_encoder_helper.h"

extern "C" {
#include "../dm_input.h"
}

#include <rtc/rtc.hpp>
#include <rtc/rtcpsrreporter.hpp>
#include <rtc/rtcpnackresponder.hpp>
#include <rtc/plihandler.hpp>
#include <rtc/frameinfo.hpp>
#include <nlohmann/json.hpp>
#include <httplib.h>

#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>
#include <future>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <libgen.h>
#endif

#pragma pack(push, 1)
struct WebRTCInputPacket {
    uint8_t magic;      // 0xAA
    uint8_t player_id;  // 0..3 (Player 1..4)
    uint16_t buttons;   // DM_BTN_* bitmask (16 bits)
    int16_t lx;         // Left stick X (-32767..32767)
    int16_t ly;         // Left stick Y (-32767..32767)
    int16_t rx;         // Right stick X (-32767..32767)
    int16_t ry;         // Right stick Y (-32767..32767)
    uint8_t has_motion; // 0 or 1
    uint8_t reserved;   // padding
    int16_t acc_x;      // acc[0] * 1000 (milligravity)
    int16_t acc_y;      // acc[1] * 1000
    int16_t acc_z;      // acc[2] * 1000
    int16_t rot_x;      // rot[0] * 1000 (mrad/s)
    int16_t rot_y;      // rot[1] * 1000
    int16_t rot_z;      // rot[2] * 1000
};
#pragma pack(pop)

struct ClientSession {
    std::string id;
    int player_id = 0;
    int h264_payload_type = 96;
    int opus_payload_type = 111;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> video_track;
    std::shared_ptr<rtc::Track> audio_track;
    std::shared_ptr<rtc::DataChannel> data_channel;
    std::atomic<bool> is_open{false};
};

static std::mutex g_clients_mu;
static std::unordered_map<std::string, std::shared_ptr<ClientSession>> g_clients;
static std::atomic<uint32_t> g_player_mask{0};

static vtb_encoder_t *g_vtb = nullptr;
static opus_helper_t *g_opus = nullptr;
static std::unique_ptr<httplib::Server> g_server;
static std::thread g_server_thread;
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
static std::unique_ptr<httplib::SSLServer> g_ssl_server;
static std::thread g_ssl_server_thread;
#endif
static std::atomic<bool> g_running{false};
static std::string g_web_dir;
static int g_port = 8888;
static int g_ssl_port = 8443;

static std::atomic<uint64_t> g_video_start_pts{0};
static std::atomic<uint32_t> g_audio_rtp_ts{0};

static void on_vtb_nalu(void *user, const uint8_t *data, size_t size, bool is_keyframe, uint64_t pts_ns) {
    (void)user;
    uint64_t start = g_video_start_pts.load(std::memory_order_relaxed);
    if (start == 0) {
        g_video_start_pts.store(pts_ns, std::memory_order_relaxed);
        start = pts_ns;
    }
    // Convert nanoseconds to 90 kHz RTP clock without jitter
    uint64_t elapsed_ns = (pts_ns >= start) ? (pts_ns - start) : 0;
    uint32_t rtp_ts = (uint32_t)((elapsed_ns * 9) / 100000);

    rtc::binary bin(reinterpret_cast<const std::byte*>(data), reinterpret_cast<const std::byte*>(data + size));
    rtc::FrameInfo info(rtp_ts);
    info.isKeyFrame = is_keyframe;

    std::lock_guard<std::mutex> lock(g_clients_mu);
    for (auto &pair : g_clients) {
        auto &client = pair.second;
        if (client->video_track && (client->video_track->isOpen() || client->is_open.load(std::memory_order_relaxed))) {
            try {
                if (client->h264_payload_type > 0) {
                    info.payloadType = (uint8_t)client->h264_payload_type;
                }
                client->video_track->sendFrame(bin, info);
            } catch (...) {}
        }
    }
}

static void on_opus_packet(void *user, const uint8_t *packet, size_t size, uint64_t pts_ns) {
    (void)user;
    (void)pts_ns;
    // Exactly 480 ticks per 10ms frame (48000 * 0.01) so NetEq never suffers clock drift / stutter
    uint32_t audio_rtp_ts = g_audio_rtp_ts.fetch_add(480, std::memory_order_relaxed);
    rtc::binary bin(reinterpret_cast<const std::byte*>(packet), reinterpret_cast<const std::byte*>(packet + size));
    rtc::FrameInfo a_info(audio_rtp_ts);

    std::lock_guard<std::mutex> lock(g_clients_mu);
    for (auto &pair : g_clients) {
        auto &client = pair.second;
        if (client->audio_track && (client->audio_track->isOpen() || client->is_open.load(std::memory_order_relaxed))) {
            try {
                if (client->opus_payload_type > 0) {
                    a_info.payloadType = (uint8_t)client->opus_payload_type;
                }
                client->audio_track->sendFrame(bin, a_info);
            } catch (...) {}
        }
    }
}

static void handle_webrtc_offer(const httplib::Request &req, httplib::Response &res) {
    try {
        auto req_json = nlohmann::json::parse(req.body);
        std::string sdp = req_json.value("sdp", "");
        std::string type = req_json.value("type", "offer");
        int requested_player = req_json.value("player", -1);

        if (sdp.empty()) {
            res.status = 400;
            res.set_content("{\"error\":\"Missing sdp\"}", "application/json");
            return;
        }

        // Tentukan player slot (0..3)
        int player_id = requested_player;
        if (player_id < 0 || player_id >= 4) {
            uint32_t current_mask = g_player_mask.load();
            for (int i = 0; i < 4; i++) {
                if (!(current_mask & (1 << i))) {
                    player_id = i;
                    break;
                }
            }
            if (player_id < 0) player_id = 0;
        }

        std::string client_id = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());

        // Evict any existing session for the same player (e.g. page reload or switching HTTP <-> HTTPS)
        {
            std::lock_guard<std::mutex> lock(g_clients_mu);
            for (auto it = g_clients.begin(); it != g_clients.end(); ) {
                if (it->second->player_id == player_id) {
                    std::cout << "[WEBRTC] Replacing stale session for Player " << (player_id + 1) << "\n";
                    if (it->second->pc) {
                        try { it->second->pc->close(); } catch (...) {}
                    }
                    it = g_clients.erase(it);
                } else {
                    ++it;
                }
            }
        }
        if (g_vtb) vtb_encoder_request_keyframe(g_vtb);

        rtc::Configuration config;
        config.enableIceTcp = false;

        auto pc = std::make_shared<rtc::PeerConnection>(config);
        auto client = std::make_shared<ClientSession>();
        client->id = client_id;
        client->player_id = player_id;
        client->pc = pc;

        rtc::Description offer_desc(sdp, type);
        std::string video_mid = "";
        std::string audio_mid = "";
        int h264_pt = 96;
        std::string h264_profile = "";
        int opus_pt = 111;
        int playout_delay_id = 0;

        for (int i = 0; i < offer_desc.mediaCount(); ++i) {
            auto m = offer_desc.media(i);
            if (std::holds_alternative<rtc::Description::Media *>(m)) {
                auto media = std::get<rtc::Description::Media *>(m);
                if (media->type() == "video" && video_mid.empty()) {
                    video_mid = media->mid();
                    for (int id : media->extIds()) {
                        const auto *ext = media->extMap(id);
                        if (ext && ext->uri == "http://www.webrtc.org/experiments/rtp-hdrext/playout-delay") {
                            playout_delay_id = id;
                            break;
                        }
                    }
                    for (int pt : media->payloadTypes()) {
                        const auto *rm = media->rtpMap(pt);
                        if (rm && (rm->format == "H264" || rm->format == "h264")) {
                            bool has_mode1 = false;
                            for (const auto &fmtp : rm->fmtps) {
                                if (fmtp.find("packetization-mode=1") != std::string::npos) {
                                    has_mode1 = true;
                                    break;
                                }
                            }
                            if (has_mode1 || h264_pt == 96) {
                                h264_pt = pt;
                                if (!rm->fmtps.empty()) {
                                    h264_profile = rm->fmtps[0];
                                }
                                if (has_mode1) break;
                            }
                        }
                    }
                } else if (media->type() == "audio" && audio_mid.empty()) {
                    audio_mid = media->mid();
                    for (int pt : media->payloadTypes()) {
                        const auto *rm = media->rtpMap(pt);
                        if (rm && (rm->format == "opus" || rm->format == "OPUS")) {
                            opus_pt = pt;
                            break;
                        }
                    }
                }
            }
        }

        client->h264_payload_type = h264_pt;
        client->opus_payload_type = opus_pt;

        std::cout << "[WEBRTC] Negotiated: video_mid=" << (video_mid.empty() ? "(none)" : video_mid)
                  << " (H264 PT " << h264_pt << "), audio_mid=" << (audio_mid.empty() ? "(none)" : audio_mid)
                  << " (Opus PT " << opus_pt << ")\n";

        // Video Track (H.264) - hanya jika client meminta video (bukan controller-only)
        if (!video_mid.empty()) {
            rtc::Description::Video video_desc(video_mid, rtc::Description::Direction::SendOnly);
            video_desc.addH264Codec(h264_pt, h264_profile.empty() ? rtc::DEFAULT_H264_VIDEO_PROFILE : h264_profile);
            video_desc.addSSRC(42 + player_id, "video-track", "stream1", "video0");

            auto video_rtp_cfg = std::make_shared<rtc::RtpPacketizationConfig>(
                42 + player_id, "video-track", h264_pt, rtc::H264RtpPacketizer::ClockRate);

            if (playout_delay_id > 0) {
                video_desc.addExtMap(rtc::Description::Entry::ExtMap(playout_delay_id, "http://www.webrtc.org/experiments/rtp-hdrext/playout-delay"));
                video_rtp_cfg->playoutDelayId = (uint8_t)playout_delay_id;
                video_rtp_cfg->playoutDelayMin = 0;
                video_rtp_cfg->playoutDelayMax = 0;
                std::cout << "[WEBRTC] Negotiated playout-delay with client extension ID " << playout_delay_id << "\n";
            }

            auto video_track = pc->addTrack(video_desc);

            auto video_packetizer = std::make_shared<rtc::H264RtpPacketizer>(
                rtc::NalUnit::Separator::StartSequence, video_rtp_cfg, 1200);

            auto srReporter = std::make_shared<rtc::RtcpSrReporter>(video_rtp_cfg);
            video_packetizer->addToChain(srReporter);
            auto nackResponder = std::make_shared<rtc::RtcpNackResponder>();
            video_packetizer->addToChain(nackResponder);

            auto pliHandler = std::make_shared<rtc::PliHandler>([]() {
                std::cout << "[WEBRTC] Receiver requested keyframe (PLI/FIR)\n";
                if (g_vtb) vtb_encoder_request_keyframe(g_vtb);
            });
            video_packetizer->addToChain(pliHandler);

            video_track->setMediaHandler(video_packetizer);
            client->video_track = video_track;

            // Request keyframe on track demand
            video_track->onOpen([video_track]() {
                std::cout << "[WEBRTC] Video track open! Requesting keyframe...\n";
                if (g_vtb) vtb_encoder_request_keyframe(g_vtb);
            });
        }

        // Audio Track (Opus) - hanya jika client meminta audio
        if (!audio_mid.empty()) {
            rtc::Description::Audio audio_desc(audio_mid, rtc::Description::Direction::SendOnly);
            audio_desc.addOpusCodec(opus_pt, rtc::DEFAULT_OPUS_AUDIO_PROFILE);
            audio_desc.addSSRC(52 + player_id, "audio-track", "stream1", "audio0");
            auto audio_track = pc->addTrack(audio_desc);

            auto audio_rtp_cfg = std::make_shared<rtc::RtpPacketizationConfig>(
                52 + player_id, "audio-track", opus_pt, rtc::OpusRtpPacketizer::DefaultClockRate);
            auto audio_packetizer = std::make_shared<rtc::OpusRtpPacketizer>(audio_rtp_cfg);

            auto srReporter = std::make_shared<rtc::RtcpSrReporter>(audio_rtp_cfg);
            audio_packetizer->addToChain(srReporter);
            auto nackResponder = std::make_shared<rtc::RtcpNackResponder>();
            audio_packetizer->addToChain(nackResponder);

            audio_track->setMediaHandler(audio_packetizer);
            client->audio_track = audio_track;
        }

        // DataChannel listener (browser membuat channel "input")
        pc->onDataChannel([client](std::shared_ptr<rtc::DataChannel> dc) {
            client->data_channel = dc;

            dc->onOpen([client]() {
                client->is_open.store(true);
                int pid = client->player_id;
                g_player_mask.fetch_or(1 << pid);
                dm_pad_players(g_player_mask.load());
                if (g_vtb) vtb_encoder_request_keyframe(g_vtb);
                std::cout << "[WEBRTC] Player " << (pid + 1) << " connected!\n";
            });

            dc->onClosed([client]() {
                client->is_open.store(false);
                int pid = client->player_id;
                g_player_mask.fetch_and(~(1 << pid));
                dm_pad_set_player(pid, 0, 0, 0, 0, 0);
                dm_pad_players(g_player_mask.load());
                std::cout << "[WEBRTC] Player " << (pid + 1) << " disconnected\n";
            });

            dc->onMessage([client](auto message) {
                if (std::holds_alternative<std::string>(message)) {
                    auto str = std::get<std::string>(message);
                    if (str == "keyframe") {
                        if (g_vtb) vtb_encoder_request_keyframe(g_vtb);
                    }
                } else if (std::holds_alternative<rtc::binary>(message)) {
                    auto bin = std::get<rtc::binary>(message);
                    if (bin.size() >= sizeof(WebRTCInputPacket)) {
                        const WebRTCInputPacket *pkt = reinterpret_cast<const WebRTCInputPacket*>(bin.data());
                        if (pkt->magic == 0xAA) {
                            int pid = client->player_id;
                            dm_pad_set_player(pid, (uint64_t)pkt->buttons, (int32_t)pkt->lx, (int32_t)pkt->ly, (int32_t)pkt->rx, (int32_t)pkt->ry);
                            if (pkt->has_motion) {
                                float acc[3] = { pkt->acc_x / 1000.0f, pkt->acc_y / 1000.0f, pkt->acc_z / 1000.0f };
                                float rot[3] = { pkt->rot_x / 1000.0f, pkt->rot_y / 1000.0f, pkt->rot_z / 1000.0f };
                                dm_motion_set(pid, acc, rot);
                            }
                        }
                    }
                }
            });
        });

        // State disconnect handler
        pc->onStateChange([client](rtc::PeerConnection::State state) {
            if (state == rtc::PeerConnection::State::Disconnected ||
                state == rtc::PeerConnection::State::Failed ||
                state == rtc::PeerConnection::State::Closed) {
                client->is_open.store(false);
                int pid = client->player_id;
                g_player_mask.fetch_and(~(1 << pid));
                dm_pad_set_player(pid, 0, 0, 0, 0, 0);
                dm_pad_players(g_player_mask.load());
                std::lock_guard<std::mutex> lock(g_clients_mu);
                g_clients.erase(client->id);
            }
        });

        // Wait for gathering complete (max 800ms)
        auto gathering_promise = std::make_shared<std::promise<void>>();
        auto gathering_future = gathering_promise->get_future();
        auto promise_fired = std::make_shared<std::atomic<bool>>(false);

        pc->onGatheringStateChange([gathering_promise, promise_fired](rtc::PeerConnection::GatheringState state) {
            if (state == rtc::PeerConnection::GatheringState::Complete) {
                if (!promise_fired->exchange(true)) {
                    gathering_promise->set_value();
                }
            }
        });

        // Set remote description
        pc->setRemoteDescription(offer_desc);

        if (pc->gatheringState() == rtc::PeerConnection::GatheringState::Complete) {
            if (!promise_fired->exchange(true)) {
                gathering_promise->set_value();
            }
        }

        {
            std::lock_guard<std::mutex> lock(g_clients_mu);
            g_clients[client_id] = client;
        }

        gathering_future.wait_for(std::chrono::milliseconds(800));

        auto local_desc = pc->localDescription();
        if (local_desc) {
            nlohmann::json resp;
            resp["sdp"] = std::string(*local_desc);
            resp["type"] = local_desc->typeString();
            resp["client_id"] = client_id;
            resp["player_id"] = player_id;
            res.set_content(resp.dump(), "application/json");
        } else {
            res.status = 500;
            res.set_content("{\"error\":\"Failed to create local description\"}", "application/json");
        }
    } catch (const std::exception &e) {
        res.status = 400;
        res.set_content(std::string("{\"error\":\"") + e.what() + "\"}", "application/json");
    }
}

static void setup_server_routes(httplib::Server &srv) {
    srv.set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type"}
    });

    srv.set_mount_point("/", g_web_dir.c_str());

    srv.Get("/api/status", [](const httplib::Request &req, httplib::Response &res) {
        (void)req;
        nlohmann::json j;
        j["running"] = true;
        j["port"] = g_port;
        j["ssl_port"] = g_ssl_port;
        j["player_mask"] = g_player_mask.load();
        std::lock_guard<std::mutex> lock(g_clients_mu);
        j["connected_clients"] = g_clients.size();
        res.set_content(j.dump(), "application/json");
    });

    srv.Post("/api/offer", [](const httplib::Request &req, httplib::Response &res) {
        handle_webrtc_offer(req, res);
    });
}

int webrtc_streamer_init(int port, const char *web_dir) {
    if (g_running.load()) return 0;
    g_port = (port > 0) ? port : 8888;
    g_ssl_port = 8443;
    if (web_dir && web_dir[0]) {
        g_web_dir = web_dir;
    } else {
#ifdef __APPLE__
        char exe_path[1024] = {0};
        uint32_t size = sizeof(exe_path);
        if (_NSGetExecutablePath(exe_path, &size) == 0) {
            char dir_buf[1024];
            strncpy(dir_buf, exe_path, sizeof(dir_buf) - 1);
            char *dir = dirname(dir_buf); // e.g. /Applications/Game.app/Contents/MacOS
            char candidate[1024];
            snprintf(candidate, sizeof(candidate), "%s/../Resources/web", dir);
            if (access(candidate, R_OK) == 0) {
                g_web_dir = candidate;
            }
        }
#endif
        if (g_web_dir.empty()) {
            if (access("dm/host/webrtc/web", R_OK) == 0) {
                g_web_dir = "dm/host/webrtc/web";
            } else if (access("/Applications/Mario Kart World.app/Contents/Resources/web", R_OK) == 0) {
                g_web_dir = "/Applications/Mario Kart World.app/Contents/Resources/web";
            } else {
                g_web_dir = "/Users/a2238/Documents/Games/nxnative/dm/host/webrtc/web";
            }
        }
    }

    // Setup VideoToolbox H.264 Encoder (480p 60fps 1.5Mbps for ultra-smooth low-latency mobile streaming)
    g_vtb = vtb_encoder_create(854, 480, 60, 1500, on_vtb_nalu, nullptr);
    if (!g_vtb) {
        std::cerr << "[WEBRTC] Failed to create VideoToolbox encoder\n";
    }

    // Setup Opus Encoder (48kHz stereo 128kbps, 10ms frame size = 480 samples)
    g_opus = opus_helper_create(48000, 2, 128, on_opus_packet, nullptr);
    if (!g_opus) {
        std::cerr << "[WEBRTC] Failed to create Opus encoder\n";
    }

    // HTTP Server (port 8888)
    g_server = std::make_unique<httplib::Server>();
    setup_server_routes(*g_server);

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    // HTTPS Server (port 8443) for Sensor / Gyro Secure Context
    if (access("/tmp/nx_cert.pem", R_OK) != 0 || access("/tmp/nx_key.pem", R_OK) != 0) {
        system("/usr/bin/openssl req -x509 -newkey rsa:2048 -keyout /tmp/nx_key.pem -out /tmp/nx_cert.pem -days 3650 -nodes -subj \"/CN=MarioKartRemote\" >/dev/null 2>&1");
    }
    if (access("/tmp/nx_cert.pem", R_OK) == 0 && access("/tmp/nx_key.pem", R_OK) == 0) {
        try {
            g_ssl_server = std::make_unique<httplib::SSLServer>("/tmp/nx_cert.pem", "/tmp/nx_key.pem");
            if (g_ssl_server->is_valid()) {
                setup_server_routes(*g_ssl_server);
            } else {
                g_ssl_server.reset();
            }
        } catch (...) {
            g_ssl_server.reset();
        }
    }
#endif

    g_running.store(true);
    g_server_thread = std::thread([]() {
        std::cout << "[WEBRTC] HTTP Server listening on http://0.0.0.0:" << g_port << "\n";
        g_server->listen("0.0.0.0", g_port);
    });

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if (g_ssl_server) {
        g_ssl_server_thread = std::thread([]() {
            std::cout << "[WEBRTC] HTTPS Server listening on https://0.0.0.0:" << g_ssl_port << "\n";
            g_ssl_server->listen("0.0.0.0", g_ssl_port);
        });
    }
#endif

    return 0;
}

void webrtc_streamer_stop(void) {
    if (!g_running.exchange(false)) return;
    if (g_server) {
        g_server->stop();
    }
    if (g_server_thread.joinable()) {
        g_server_thread.join();
    }
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if (g_ssl_server) {
        g_ssl_server->stop();
    }
    if (g_ssl_server_thread.joinable()) {
        g_ssl_server_thread.join();
    }
#endif
    {
        std::lock_guard<std::mutex> lock(g_clients_mu);
        g_clients.clear();
    }
    if (g_vtb) {
        vtb_encoder_destroy(g_vtb);
        g_vtb = nullptr;
    }
    if (g_opus) {
        opus_helper_destroy(g_opus);
        g_opus = nullptr;
    }
    g_player_mask.store(0);
    dm_pad_players(0);
}

int webrtc_streamer_active(void) {
    return g_running.load(std::memory_order_relaxed) ? 1 : 0;
}

int webrtc_streamer_wants_video(void) {
    if (!g_running.load(std::memory_order_relaxed) || !g_vtb) return 0;
    std::lock_guard<std::mutex> lock(g_clients_mu);
    for (auto &pair : g_clients) {
        if (pair.second->video_track) {
            return 1;
        }
    }
    return 0;
}

int webrtc_streamer_wants_audio(void) {
    if (!g_running.load(std::memory_order_relaxed) || !g_opus) return 0;
    std::lock_guard<std::mutex> lock(g_clients_mu);
    for (auto &pair : g_clients) {
        if (pair.second->audio_track) {
            return 1;
        }
    }
    return 0;
}

int webrtc_streamer_client_count(void) {
    std::lock_guard<std::mutex> lock(g_clients_mu);
    return (int)g_clients.size();
}

void webrtc_streamer_send_metal_texture(void *mtl_texture, void *mtl_dev, void *mtl_cmd_queue, uint64_t pts_ns) {
    if (!webrtc_streamer_wants_video()) return; // 0ms / 0 GPU cost jika tidak ada yang meminta video
    vtb_encoder_encode_metal(g_vtb, mtl_texture, mtl_dev, mtl_cmd_queue, pts_ns);
}

void webrtc_streamer_send_pcm_float(const float *samples, size_t frames, int channels) {
    if (!webrtc_streamer_wants_audio()) return;
    if (channels == 2) {
        uint64_t pts_ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        opus_helper_feed_float(g_opus, samples, frames, pts_ns);
    }
}

void webrtc_streamer_send_pcm_int16(const int16_t *samples, size_t frames, int channels) {
    if (!webrtc_streamer_wants_audio()) return;
    if (channels == 2) {
        uint64_t pts_ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        opus_helper_feed_int16(g_opus, samples, frames, pts_ns);
    }
}
