#include <coop/io.hpp>
#include <coop/task-handle.hpp>

#include "gstutil/auto-gst-object.hpp"
#include "gstutil/caps.hpp"
#include "gstutil/pipeline-helper.hpp"
#include "macros/autoptr.hpp"
#include "macros/logger.hpp"
#include "net/packet-parser.hpp"
#include "net/tcp/server.hpp"
#include "protocol.hpp"
#include "util/argument-parser.hpp"
#include "util/event.hpp"

#define CUTIL_MACROS_PRINT_FUNC(...) LOG_ERROR(logger, __VA_ARGS__)
#include "macros/coop-unwrap.hpp"

namespace {
constexpr auto testsrc  = false;
constexpr auto loopback = false;

declare_autoptr(GMainLoop, GMainLoop, g_main_loop_unref);
declare_autoptr(GstMessage, GstMessage, gst_message_unref);
declare_autoptr(GString, gchar, g_free);

auto logger = Logger("CAMERA");

struct ClientData {
    net::PacketParser parser;
};

struct GstContext {
    const ClientData*         owner = nullptr;
    AutoGstObject<GstElement> pipeline;
    GstElement*               static_elements_head_tail;
    GstElement*               static_elements_tail_head;
    GstElement*               x264enc = nullptr;
    std::vector<GstElement*>  dynamic_elements;
};

struct PipeConfig {
    std::array<uint32_t, 3> hw_res = {1280, 720, 30};
    std::array<uint32_t, 3> sw_res = {1280, 720, 30};
};

auto pipe_config = PipeConfig();

auto setup_dynamic_elements(GstContext& ctx) -> bool {
    const auto pipeline = ctx.pipeline.get();

    // videorate ! videoscale ! (capsfilter) ! videoconvert ! x264enc
    unwrap_mut(videorate, add_new_element_to_pipeine(pipeline, "videorate"));
    g_object_set(&videorate, "max-rate", pipe_config.sw_res[2], "skip-to-first", TRUE, NULL);
    unwrap_mut(videoscale, add_new_element_to_pipeine(pipeline, "videoscale"));
    unwrap_mut(capsfilter2, add_new_element_to_pipeine(pipeline, "capsfilter"));
    ensure(set_caps(&capsfilter2, std::format("video/x-raw,width={},height={}", pipe_config.sw_res[0], pipe_config.sw_res[1]).data()));
    unwrap_mut(videoconvert, add_new_element_to_pipeine(pipeline, "videoconvert"));
    if(loopback) {
        unwrap_mut(waylandsink, add_new_element_to_pipeine(pipeline, "waylandsink"));
        g_object_set(&waylandsink, "async", FALSE, "sync", FALSE, "qos", FALSE, NULL);
        ctx.dynamic_elements = {&videorate, &videoscale, &capsfilter2, &videoconvert, &waylandsink};
    } else {
        unwrap_mut(x264enc, add_new_element_to_pipeine(pipeline, "x264enc"));
        g_object_set(&x264enc, "speed-preset", 1 /*ultrafast*/, "tune", 4 | 2, "key-int-max", 30, NULL);
        ctx.dynamic_elements = {&videorate, &videoscale, &capsfilter2, &videoconvert, &x264enc};
        ctx.x264enc          = &x264enc; // save for bitrate command
    }

    auto& dyn = ctx.dynamic_elements;
    for(auto i = 0uz; i < dyn.size() - 1; i += 1) {
        ensure(gst_element_link(dyn[i], dyn[i + 1]));
        ensure(gst_element_sync_state_with_parent(dyn[i]) == TRUE);
    }
    ensure(gst_element_sync_state_with_parent(dyn.back()) == TRUE);

    return true;
}

auto cleanup_dynamic_elements(GstContext& ctx) -> bool {
    for(const auto e : ctx.dynamic_elements) {
        ensure(gst_element_set_state(e, GST_STATE_NULL) == GST_STATE_CHANGE_SUCCESS);
        ensure(gst_bin_remove(GST_BIN(ctx.pipeline.get()), e) == TRUE);
    }
    ctx.dynamic_elements.clear();
    return true;
}

auto replace_dynamic_elements(GstContext& ctx) -> bool {
    ensure(cleanup_dynamic_elements(ctx));
    ensure(setup_dynamic_elements(ctx));
    ensure(gst_element_link(ctx.static_elements_head_tail, ctx.dynamic_elements.front()) == TRUE);
    if(!loopback) {
        ensure(gst_element_link(ctx.dynamic_elements.back(), ctx.static_elements_tail_head) == TRUE);
    }
    return true;
}

auto gst_context = std::optional<GstContext>();

auto start_streaming(const ClientData& owner, const char* const data_addr, uint16_t data_port) -> bool {
    LOG_INFO(logger, "starting streaming to {}:{}", data_addr, data_port);

    auto ctx     = GstContext();
    ctx.owner    = &owner;
    ctx.pipeline = AutoGstObject(gst_pipeline_new(NULL));
    ensure(ctx.pipeline);
    const auto pipeline = ctx.pipeline.get();

    // v4l2src ! (capsfilter) ! jpegdec ! (dynamic elements) ! rtph264pay ! udpsink

    // static elements head
    if(testsrc) {
        unwrap_mut(v4l2src, add_new_element_to_pipeine(pipeline, "videotestsrc"));
        g_object_set(&v4l2src, "is-live", TRUE, "pattern", 11, "horizontal-speed", 2, NULL);
        ctx.static_elements_head_tail = &v4l2src;
        const auto src_pad            = AutoGstObject(gst_element_get_static_pad(&v4l2src, "src"));
    } else {
        unwrap_mut(v4l2src, add_new_element_to_pipeine(pipeline, "v4l2src"));
        g_object_set(&v4l2src, "device", "/dev/video0", NULL);
        unwrap_mut(capsfilter1, add_new_element_to_pipeine(pipeline, "capsfilter"));
        ensure(set_caps(&capsfilter1, std::format("image/jpeg,width={},height={},framerate={}/1", pipe_config.hw_res[0], pipe_config.hw_res[1], pipe_config.hw_res[2]).data()));
        unwrap_mut(jpegdec, add_new_element_to_pipeine(pipeline, "jpegdec"));
        ctx.static_elements_head_tail = &jpegdec;
        ensure(gst_element_link_many(&v4l2src, &capsfilter1, &jpegdec, NULL) == TRUE);
        const auto src_pad = AutoGstObject(gst_element_get_static_pad(&jpegdec, "src"));
    }

    // static elements tail
    unwrap_mut(rtph264pay, add_new_element_to_pipeine(pipeline, "rtph264pay"));
    unwrap_mut(udpsink, add_new_element_to_pipeine(pipeline, "udpsink"));
    g_object_set(&udpsink, "host", data_addr, "port", data_port, "async", FALSE, NULL);
    ctx.static_elements_tail_head = &rtph264pay;
    ensure(gst_element_link_many(&rtph264pay, &udpsink, NULL) == TRUE);

    // dynamic elements
    ensure(replace_dynamic_elements(ctx));

    ensure(gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_SUCCESS);

    gst_context.emplace(std::move(ctx));

    LOG_INFO(logger, "start done");
    return true;
}

auto stop_streaming() -> bool {
    LOG_INFO(logger, "stopping streaming");

    const auto pipeline = gst_context->pipeline.get();
    ensure(post_eos(pipeline));
    ensure(gst_element_set_state(pipeline, GST_STATE_NULL) == GST_STATE_CHANGE_SUCCESS);
    gst_context.reset();

    LOG_INFO(logger, "stop done");
    return true;
}

auto pad_unblocked = Event();

auto on_pad_block(GstPad* const /*pad*/, GstPadProbeInfo* const /*info*/, gpointer const /*data*/) -> GstPadProbeReturn {
    auto& ctx = *gst_context;
    LOG_DEBUG(logger, "blocked");
    ASSERT(replace_dynamic_elements(ctx));
    pad_unblocked.notify();
    LOG_DEBUG(logger, "done");
    return GST_PAD_PROBE_REMOVE;
}

auto ipv4_to_string(const uint32_t addr) -> std::string {
    const auto [a, b, c, d] = std::bit_cast<std::array<uint8_t, 4>>(addr);
    return std::format("{}.{}.{}.{}", d, c, b, a);
}

auto handle_payload(const net::ClientData& client_data, const net::Header header, const net::BytesRef payload) -> coop::Async<bool> {
    auto& client = *(ClientData*)client_data.data;
    switch(header.type) {
    case proto::StartStreaming::pt: {
        coop_unwrap_mut(request, (serde::load<net::BinaryFormat, proto::StartStreaming>(payload)));
        coop_ensure(!gst_context);
        coop_unwrap(addr, net::tcp::TCPServerBackend::get_peer_addr_ipv4(client_data));
        coop_ensure(start_streaming(client, ipv4_to_string(addr).data(), request.port));
        goto success;
    }
    case proto::ChangeResolution::pt: {
        coop_unwrap_mut(request, (serde::load<net::BinaryFormat, proto::ChangeResolution>(payload)));
        coop_ensure(gst_context);
        pipe_config.sw_res[0] = request.width;
        pipe_config.sw_res[1] = request.height;
        goto reconfigure;
    }
    case proto::ChangeFramerate::pt: {
        coop_unwrap_mut(request, (serde::load<net::BinaryFormat, proto::ChangeFramerate>(payload)));
        coop_ensure(gst_context);
        pipe_config.sw_res[2] = request.framerate;
        goto reconfigure;
    }
    case proto::ChangeBitrate::pt: {
        coop_unwrap_mut(request, (serde::load<net::BinaryFormat, proto::ChangeBitrate>(payload)));
        coop_ensure(gst_context);
        coop_ensure(gst_context->x264enc);
        g_object_set(gst_context->x264enc, "bitrate", request.bitrate, NULL);
        goto success;
    }
    default:
        coop_bail("unhandled packet type {}", header.type);
    }
reconfigure: {
    const auto src_pad = AutoGstObject(gst_element_get_static_pad(gst_context->static_elements_head_tail, "src"));
    gst_pad_add_probe(src_pad.get(), GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM, on_pad_block, NULL, NULL);
    pad_unblocked.wait(); // intentionally blocking runner
}
success: {
    coop_ensure(co_await client.parser.send_packet(proto::Success(), header.id));
    co_return true;
}
}

auto port = 8080;

auto async_main() -> coop::Async<bool> {
    // setup control socket
    auto server         = net::tcp::TCPServerBackend();
    server.alloc_client = [&server](net::ClientData& client) -> coop::Async<void> {
        auto& cd            = *new ClientData();
        client.data         = &cd;
        cd.parser.send_data = [&server, &client](const net::BytesRef payload) -> coop::Async<bool> {
            constexpr auto error_value = false;
            co_ensure_v(co_await server.send(client, payload));
            co_return true;
        };
        co_return;
    };
    server.free_client = [](void* ptr) -> coop::Async<void> {
        auto& client = *(ClientData*)ptr;
        delete &client;
        if(gst_context && gst_context->owner == &client) {
            coop_ensure(stop_streaming());
        }
        co_return;
    };
    server.on_received = [](const net::ClientData& client, net::BytesRef data) -> coop::Async<void> {
        auto& c = *(ClientData*)client.data;
        if(const auto p = c.parser.parse_received(data)) {
            if(!co_await handle_payload(client, p->header, p->payload)) {
                co_await c.parser.send_packet(proto::Error(), p->header.id);
            }
        }
    };
    coop_ensure(co_await server.start(port));

    // wait until finished
    co_await server.task.join();
    co_return true;
}
} // namespace

auto main(const int argc, const char* const* argv) -> int {
    {
        auto parser = args::Parser<uint16_t>();
        auto help   = false;
        parser.kwarg(&port, {"-p", "--port"}, "PORT", "port number to use", {.state = args::State::DefaultValue});
        parser.kwflag(&help, {"-h", "--help"}, "print this help message", {.no_error_check = true});
        if(!parser.parse(argc, argv) || help) {
            std::println("usage: camera {}", parser.get_help());
            return 0;
        }
    }

    gst_init(NULL, NULL);
    auto runner = coop::Runner();
    runner.push_task(async_main());
    runner.run();
    return 0;
}
