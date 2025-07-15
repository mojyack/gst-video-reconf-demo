#include <iostream>

#include <coop/io.hpp>
#include <coop/task-handle.hpp>

#include "gstutil/auto-gst-object.hpp"
#include "gstutil/caps.hpp"
#include "gstutil/pipeline-helper.hpp"
#include "macros/logger.hpp"
#include "net/packet-parser.hpp"
#include "net/tcp/client.hpp"
#include "protocol.hpp"
#include "util/argument-parser.hpp"
#include "util/split.hpp"

#define CUTIL_MACROS_PRINT_FUNC(...) LOG_ERROR(logger, __VA_ARGS__)
#include "macros/coop-unwrap.hpp"

namespace {
auto logger = Logger("DISPLAY");

auto cam_addr = "127.0.0.1";
auto cam_port = 8080;

auto async_main() -> coop::Async<bool> {
    auto parser       = net::PacketParser();
    auto control_sock = net::tcp::TCPClientBackend();

    parser.send_data = [&control_sock](const net::BytesRef payload) -> coop::Async<bool> {
        constexpr auto error_value = false;
        co_ensure_v(co_await control_sock.send(payload));
        co_return true;
    };
    control_sock.on_received = [&parser](net::BytesRef data) -> coop::Async<void> {
        if(const auto p = parser.parse_received(data)) {
            const auto [header, payload] = *p;
            coop_ensure(co_await parser.callbacks.invoke(header, payload));
        }
        co_return;
    };
    control_sock.on_closed = [] {
        LOG_WARN(logger, "disconnected");
        std::quick_exit(1);
    };
    coop_ensure(co_await control_sock.connect(cam_addr, cam_port));

    coop_ensure(co_await parser.receive_response<proto::Success>(proto::StartStreaming()));

    // udpsrc ! rtpjitterbuffer ! rtph264depay ! avdec_h264 ! videoconvert !waylandsink
    auto pipeline = AutoGstObject(gst_pipeline_new(NULL));
    coop_ensure(pipeline);
    coop_unwrap_mut(udpsrc, add_new_element_to_pipeine(pipeline.get(), "udpsrc"));
    g_object_set(&udpsrc, "port", 8081, NULL);
    coop_unwrap_mut(capsfilter1, add_new_element_to_pipeine(pipeline.get(), "capsfilter"));
    set_caps(&capsfilter1, "application/x-rtp,media=video");
    coop_unwrap_mut(rtpjitterbuffer, add_new_element_to_pipeine(pipeline.get(), "rtpjitterbuffer"));
    coop_unwrap_mut(rtph264depay, add_new_element_to_pipeine(pipeline.get(), "rtph264depay"));
    coop_unwrap_mut(avdec_h264, add_new_element_to_pipeine(pipeline.get(), "avdec_h264"));
    g_object_set(&avdec_h264, "qos", FALSE, NULL);
    coop_unwrap_mut(videoconvert, add_new_element_to_pipeine(pipeline.get(), "videoconvert"));
    coop_unwrap_mut(waylandsink, add_new_element_to_pipeine(pipeline.get(), "waylandsink"));
    g_object_set(&waylandsink, "async", FALSE, NULL);

    coop_ensure(gst_element_link_many(&udpsrc, &capsfilter1, &rtpjitterbuffer, &rtph264depay, &avdec_h264, &videoconvert, &waylandsink, NULL) == TRUE);
    coop_ensure(gst_element_set_state(pipeline.get(), GST_STATE_PLAYING) == GST_STATE_CHANGE_SUCCESS);

loop:
    std::print("> ");
    std::flush(std::cout);
    coop_ensure(!(co_await coop::wait_for_file(fileno(stdin), true, false)).error);
    auto line = std::string();
    coop_ensure(std::getline(std::cin, line));
    if(line.empty()) {
        goto loop;
    }
    const auto elms = split(line, " ");
#define error_act goto loop
    ensure_a(elms.size() >= 1);
    const auto command = elms[0];
    if(command == "r" || command == "res") {
        ensure_a(elms.size() == 3);
        unwrap_a(width, from_chars<uint32_t>(elms[1]));
        unwrap_a(height, from_chars<uint32_t>(elms[1]));
        LOG_INFO(logger, "changing to {}x{}", width, height);
        ensure_a(co_await parser.receive_response<proto::Success>(proto::ChangeResolution{width, height}));
    } else if(command == "f" || command == "framerate") {
        ensure_a(elms.size() == 2);
        unwrap_a(num, from_chars<uint32_t>(elms[1]));
        LOG_INFO(logger, "changing to @{}", num);
        ensure_a(co_await parser.receive_response<proto::Success>(proto::ChangeFramerate{num}));
    } else if(command == "b" || command == "bitrate") {
        ensure_a(elms.size() == 2);
        unwrap_a(num, from_chars<uint32_t>(elms[1]));
        LOG_INFO(logger, "changing to {}kbps", num);
        ensure_a(co_await parser.receive_response<proto::Success>(proto::ChangeBitrate{num}));
    } else if(command == "q" || command == "quit" || command == "exit") {
        co_return true;
    } else {
        std::println("commands:");
        std::println("r|res WIDTH HEIGHT      change resolution");
        std::println("f|framerate FRAMERATE   change framerate");
        std::println("b|bitrate BITRATE       change bitrate in kbit/sec");
        std::println("q|quit|exit             exit display app");
        goto loop;
    }
    std::println("done");
#undef error_act
    goto loop;
}
} // namespace

auto main(const int argc, const char* const* argv) -> int {
    {
        auto parser = args::Parser<uint16_t>();
        auto help   = false;
        parser.kwarg(&cam_addr, {"-a", "--addr"}, "ADDRESS", "camera port address", {.state = args::State::DefaultValue});
        parser.kwarg(&cam_port, {"-p", "--port"}, "PORT", "camera port number", {.state = args::State::DefaultValue});
        parser.kwflag(&help, {"-h", "--help"}, "print this help message", {.no_error_check = true});
        if(!parser.parse(argc, argv) || help) {
            std::println("usage: display {}", parser.get_help());
            return 0;
        }
    }

    gst_init(NULL, NULL);
    auto runner = coop::Runner();
    runner.push_task(async_main());
    runner.run();
    return 0;
}
