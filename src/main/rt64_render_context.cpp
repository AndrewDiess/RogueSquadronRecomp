#include <memory>
#include <cstring>
#include <variant>
#include <algorithm>
#include <filesystem>
#include <cstdio>
#include <vector>

#define HLSL_CPU
#include "hle/rt64_application.h"
#include "rt64_render_hooks.h"
#include "overloaded.h"

#include "ultramodern/ultramodern.hpp"
#include "ultramodern/config.hpp"

#include "zelda_render.h"
#include "recomp_ui.h"
#include "concurrentqueue.h"

static RT64::UserConfiguration::Antialiasing device_max_msaa = RT64::UserConfiguration::Antialiasing::None;
static bool sample_positions_supported = false;
static bool high_precision_fb_enabled = false;

static uint8_t DMEM[0x1000];
static uint8_t IMEM[0x1000];

struct TexturePackEnableAction {
    std::string mod_id;
};

struct TexturePackDisableAction {
    std::string mod_id;
};

struct TexturePackSecondaryEnableAction {
    std::string mod_id;
};

struct TexturePackSecondaryDisableAction {
    std::string mod_id;
};

struct TexturePackUpdateAction {
};

using TexturePackAction = std::variant<TexturePackEnableAction, TexturePackDisableAction, TexturePackSecondaryEnableAction, TexturePackSecondaryDisableAction, TexturePackUpdateAction>;

static moodycamel::ConcurrentQueue<TexturePackAction> texture_pack_action_queue;

unsigned int MI_INTR_REG = 0;

unsigned int DPC_START_REG = 0;
unsigned int DPC_END_REG = 0;
unsigned int DPC_CURRENT_REG = 0;
unsigned int DPC_STATUS_REG = 0;
unsigned int DPC_CLOCK_REG = 0;
unsigned int DPC_BUFBUSY_REG = 0;
unsigned int DPC_PIPEBUSY_REG = 0;
unsigned int DPC_TMEM_REG = 0;

void dummy_check_interrupts() {}

RT64::UserConfiguration::Antialiasing compute_max_supported_aa(plume::RenderSampleCounts bits) {
    if (bits & plume::RenderSampleCount::Bits::COUNT_2) {
        if (bits & plume::RenderSampleCount::Bits::COUNT_4) {
            if (bits & plume::RenderSampleCount::Bits::COUNT_8) {
                return RT64::UserConfiguration::Antialiasing::MSAA8X;
            }
            return RT64::UserConfiguration::Antialiasing::MSAA4X;
        }
        return RT64::UserConfiguration::Antialiasing::MSAA2X;
    };
    return RT64::UserConfiguration::Antialiasing::None;
}

RT64::UserConfiguration::AspectRatio to_rt64(ultramodern::renderer::AspectRatio option) {
    switch (option) {
        case ultramodern::renderer::AspectRatio::Original:
            return RT64::UserConfiguration::AspectRatio::Original;
        case ultramodern::renderer::AspectRatio::Expand:
            return RT64::UserConfiguration::AspectRatio::Expand;
        case ultramodern::renderer::AspectRatio::Manual:
            return RT64::UserConfiguration::AspectRatio::Manual;
        case ultramodern::renderer::AspectRatio::OptionCount:
            return RT64::UserConfiguration::AspectRatio::OptionCount;
    }
}

RT64::UserConfiguration::Antialiasing to_rt64(ultramodern::renderer::Antialiasing option) {
    switch (option) {
        case ultramodern::renderer::Antialiasing::None:
            return RT64::UserConfiguration::Antialiasing::None;
        case ultramodern::renderer::Antialiasing::MSAA2X:
            return RT64::UserConfiguration::Antialiasing::MSAA2X;
        case ultramodern::renderer::Antialiasing::MSAA4X:
            return RT64::UserConfiguration::Antialiasing::MSAA4X;
        case ultramodern::renderer::Antialiasing::MSAA8X:
            return RT64::UserConfiguration::Antialiasing::MSAA8X;
        case ultramodern::renderer::Antialiasing::OptionCount:
            return RT64::UserConfiguration::Antialiasing::OptionCount;
    }
}

RT64::UserConfiguration::RefreshRate to_rt64(ultramodern::renderer::RefreshRate option) {
    switch (option) {
        case ultramodern::renderer::RefreshRate::Original:
            return RT64::UserConfiguration::RefreshRate::Original;
        case ultramodern::renderer::RefreshRate::Display:
            return RT64::UserConfiguration::RefreshRate::Display;
        case ultramodern::renderer::RefreshRate::Manual:
            return RT64::UserConfiguration::RefreshRate::Manual;
        case ultramodern::renderer::RefreshRate::OptionCount:
            return RT64::UserConfiguration::RefreshRate::OptionCount;
    }
}

RT64::UserConfiguration::InternalColorFormat to_rt64(ultramodern::renderer::HighPrecisionFramebuffer option) {
    switch (option) {
        case ultramodern::renderer::HighPrecisionFramebuffer::Off:
            return RT64::UserConfiguration::InternalColorFormat::Standard;
        case ultramodern::renderer::HighPrecisionFramebuffer::On:
            return RT64::UserConfiguration::InternalColorFormat::High;
        case ultramodern::renderer::HighPrecisionFramebuffer::Auto:
            return RT64::UserConfiguration::InternalColorFormat::Automatic;
        case ultramodern::renderer::HighPrecisionFramebuffer::OptionCount:
            return RT64::UserConfiguration::InternalColorFormat::OptionCount;
    }
}

void set_application_user_config(RT64::Application* application, const ultramodern::renderer::GraphicsConfig& config) {
    switch (config.res_option) {
        default:
        case ultramodern::renderer::Resolution::Auto:
            application->userConfig.resolution = RT64::UserConfiguration::Resolution::WindowIntegerScale;
            application->userConfig.downsampleMultiplier = 1;
            break;
        case ultramodern::renderer::Resolution::Original:
            application->userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
            application->userConfig.resolutionMultiplier = std::max(config.ds_option, 1);
            application->userConfig.downsampleMultiplier = std::max(config.ds_option, 1);
            break;
        case ultramodern::renderer::Resolution::Original2x:
            application->userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
            application->userConfig.resolutionMultiplier = 2.0 * std::max(config.ds_option, 1);
            application->userConfig.downsampleMultiplier = std::max(config.ds_option, 1);
            break;
    }

    switch (config.hr_option) {
        default:
        case ultramodern::renderer::HUDRatioMode::Original:
            application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Original;
            break;
        case ultramodern::renderer::HUDRatioMode::Clamp16x9:
            application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Manual;
            application->userConfig.extAspectTarget = 16.0/9.0;
            break;
        case ultramodern::renderer::HUDRatioMode::Full:
            application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Expand;
            break;
    }

    application->userConfig.aspectRatio = to_rt64(config.ar_option);
    application->userConfig.antialiasing = to_rt64(config.msaa_option);
    application->userConfig.refreshRate = to_rt64(config.rr_option);
    application->userConfig.refreshRateTarget = config.rr_manual_value;
    application->userConfig.internalColorFormat = to_rt64(config.hpfb_option);
    application->userConfig.displayBuffering = RT64::UserConfiguration::DisplayBuffering::Triple;
}

ultramodern::renderer::SetupResult map_setup_result(RT64::Application::SetupResult rt64_result) {
    switch (rt64_result) {
        case RT64::Application::SetupResult::Success:
            return ultramodern::renderer::SetupResult::Success;
        case RT64::Application::SetupResult::DynamicLibrariesNotFound:
            return ultramodern::renderer::SetupResult::DynamicLibrariesNotFound;
        case RT64::Application::SetupResult::InvalidGraphicsAPI:
            return ultramodern::renderer::SetupResult::InvalidGraphicsAPI;
        case RT64::Application::SetupResult::GraphicsAPINotFound:
            return ultramodern::renderer::SetupResult::GraphicsAPINotFound;
        case RT64::Application::SetupResult::GraphicsDeviceNotFound:
            return ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
    }

    fprintf(stderr, "Unhandled `RT64::Application::SetupResult` ?\n");
    assert(false);
    std::exit(EXIT_FAILURE);
}

ultramodern::renderer::GraphicsApi map_graphics_api(RT64::UserConfiguration::GraphicsAPI api) {
    switch (api) {
        case RT64::UserConfiguration::GraphicsAPI::D3D12:
            return ultramodern::renderer::GraphicsApi::D3D12;
        case RT64::UserConfiguration::GraphicsAPI::Vulkan:
            return ultramodern::renderer::GraphicsApi::Vulkan;
        case RT64::UserConfiguration::GraphicsAPI::Metal:
            return ultramodern::renderer::GraphicsApi::Metal;
        case RT64::UserConfiguration::GraphicsAPI::Automatic:
            return ultramodern::renderer::GraphicsApi::Auto;
    }

    fprintf(stderr, "Unhandled `RT64::UserConfiguration::GraphicsAPI` ?\n");
    assert(false);
    std::exit(EXIT_FAILURE);
}

zelda64::renderer::RT64Context::RT64Context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool debug) {
    static unsigned char dummy_rom_header[0x40];
    recompui::set_render_hooks();

    // Set up the RT64 application core fields.
    RT64::Application::Core appCore{};
#if defined(_WIN32)
    appCore.window = window_handle.window;
#elif defined(__linux__) || defined(__ANDROID__)
    appCore.window = window_handle;
#elif defined(__APPLE__)
    appCore.window.window = window_handle.window;
    appCore.window.view = window_handle.view;
#endif

    appCore.checkInterrupts = dummy_check_interrupts;

    appCore.HEADER = dummy_rom_header;
    appCore.RDRAM = rdram;
    appCore.DMEM = DMEM;
    appCore.IMEM = IMEM;

    appCore.MI_INTR_REG = &MI_INTR_REG;

    appCore.DPC_START_REG = &DPC_START_REG;
    appCore.DPC_END_REG = &DPC_END_REG;
    appCore.DPC_CURRENT_REG = &DPC_CURRENT_REG;
    appCore.DPC_STATUS_REG = &DPC_STATUS_REG;
    appCore.DPC_CLOCK_REG = &DPC_CLOCK_REG;
    appCore.DPC_BUFBUSY_REG = &DPC_BUFBUSY_REG;
    appCore.DPC_PIPEBUSY_REG = &DPC_PIPEBUSY_REG;
    appCore.DPC_TMEM_REG = &DPC_TMEM_REG;

    ultramodern::renderer::ViRegs* vi_regs = ultramodern::renderer::get_vi_regs();

    appCore.VI_STATUS_REG = &vi_regs->VI_STATUS_REG;
    appCore.VI_ORIGIN_REG = &vi_regs->VI_ORIGIN_REG;
    appCore.VI_WIDTH_REG = &vi_regs->VI_WIDTH_REG;
    appCore.VI_INTR_REG = &vi_regs->VI_INTR_REG;
    appCore.VI_V_CURRENT_LINE_REG = &vi_regs->VI_V_CURRENT_LINE_REG;
    appCore.VI_TIMING_REG = &vi_regs->VI_TIMING_REG;
    appCore.VI_V_SYNC_REG = &vi_regs->VI_V_SYNC_REG;
    appCore.VI_H_SYNC_REG = &vi_regs->VI_H_SYNC_REG;
    appCore.VI_LEAP_REG = &vi_regs->VI_LEAP_REG;
    appCore.VI_H_START_REG = &vi_regs->VI_H_START_REG;
    appCore.VI_V_START_REG = &vi_regs->VI_V_START_REG;
    appCore.VI_V_BURST_REG = &vi_regs->VI_V_BURST_REG;
    appCore.VI_X_SCALE_REG = &vi_regs->VI_X_SCALE_REG;
    appCore.VI_Y_SCALE_REG = &vi_regs->VI_Y_SCALE_REG;

    // Set up the RT64 application configuration fields.
    RT64::ApplicationConfiguration appConfig;
    appConfig.useConfigurationFile = false;

    // Create the RT64 application.
    app = std::make_unique<RT64::Application>(appCore, appConfig);

    // Set initial user config settings based on the current settings.
    auto& cur_config = ultramodern::renderer::get_graphics_config();
    set_application_user_config(app.get(), cur_config);
    app->userConfig.developerMode = debug;
    printf("[swrs] RT64 developer mode: %s\n", debug ? "on" : "off");
    // Force gbi depth branches to prevent LODs from kicking in.
    app->enhancementConfig.f3dex.forceBranch = true;
    // Scale LODs based on the output resolution.
    app->enhancementConfig.textureLOD.scale = true;
    // Pick an API if the user has set an override.
    switch (cur_config.api_option) {
        case ultramodern::renderer::GraphicsApi::D3D12:
            app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::D3D12;
            break;
        case ultramodern::renderer::GraphicsApi::Vulkan:
            app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Vulkan;
            break;
        case ultramodern::renderer::GraphicsApi::Metal:
            app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Metal;
            break;
        case ultramodern::renderer::GraphicsApi::Auto:
#ifdef _WIN32
            // Rogue Squadron: on D3D12 the GPU hangs (DXGI_ERROR_DEVICE_HUNG, TDR) in roughly one
            // run in three when the mission intro cutscene starts after the opening crawl; the
            // D3D12 debug layer reports no API misuse. Vulkan ran 8 of 8 times without it, so
            // Auto picks Vulkan here. D3D12 can still be chosen explicitly.
            app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Vulkan;
#else
            app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Automatic;
#endif
            break;
    }

    // Set up the RT64 application.
    uint32_t thread_id = 0;
#ifdef _WIN32
    thread_id = window_handle.thread_id;
#endif
    setup_result = map_setup_result(app->setup(thread_id));
    // Get the API that RT64 chose.
    chosen_api = map_graphics_api(app->chosenGraphicsAPI);
    if (setup_result != ultramodern::renderer::SetupResult::Success) {
        app = nullptr;
        return;
    }

    // Set the application's fullscreen state.
    app->setFullScreen(cur_config.wm_option == ultramodern::renderer::WindowMode::Fullscreen);

    // Check if the selected device actually supports MSAA sample positions and MSAA for for the formats that will be used
    // and downgrade the configuration accordingly.
    if (app->device->getCapabilities().sampleLocations) {
        plume::RenderSampleCounts color_sample_counts = app->device->getSampleCountsSupported(plume::RenderFormat::R8G8B8A8_UNORM);
        plume::RenderSampleCounts depth_sample_counts = app->device->getSampleCountsSupported(plume::RenderFormat::D32_FLOAT);
        plume::RenderSampleCounts common_sample_counts = color_sample_counts & depth_sample_counts;
        device_max_msaa = compute_max_supported_aa(common_sample_counts);
        sample_positions_supported = true;
    }
    else {
        device_max_msaa = RT64::UserConfiguration::Antialiasing::None;
        sample_positions_supported = false;
    }

    high_precision_fb_enabled = app->shaderLibrary->usesHDR;
}

zelda64::renderer::RT64Context::~RT64Context() = default;

// Bring-up counter, read by the --start monitor in main.cpp.
std::atomic<uint32_t> swrs_gfx_task_count{0};
// Bring-up switch for RDP command logging, defined in RT64 (rt64_rdp.cpp).
extern int swrs_rdp_log;

// Rogue Squadron: keep the original 4:3 picture outside in-flight gameplay even with Expand selected (the intro,
// cutscenes and front end are 4:3 content). Set from the game glue when an overlay loads.
static std::atomic<bool> swrs_force_original_aspect{ true };
void swrs_set_force_original_aspect(bool force) {
    swrs_force_original_aspect = force;
}

// How much wider than 4:3 the picture is with Expand (1 otherwise). The game's object culling is widened by this
// factor (swrs_cull_widen in swrs_game.cpp) so objects in the extended side area are drawn.
std::atomic<float> swrs_cull_aspect_scale{ 1.0f };
void swrs_check_ram_dump(uint8_t* rdram); // swrs_game.cpp

// Rogue Squadron's in-mission cutscenes (all missions) letterbox by shrinking the game's 3D view rectangle inside the
// screen. The only caller of the setter (FUN_800129F4) is the in-mission cutscene start FUN_800ECEB8; it stores left,
// top, right, bottom halfwords at 0x8010C478..0x8010C47E and sets the enable byte 0x8010C484 (cleared again by
// FUN_80012A40 and the 3D setup FUN_80011DB8). FUN_800093A8 fills the area outside the rectangle with black bars.
// Screen width and height are the words at 0x8010C430 / 0x8010C434. True while that letterbox is on, so those shots
// stay 4:3 like the other cutscenes.
static bool swrs_view_letterboxed(const uint8_t* rdram) {
    const uint8_t enabled = rdram[(0x8010C484U - 0x80000000U) ^ 3U];
    if (enabled == 0) {
        return false;
    }

    auto u16 = [rdram](uint32_t address) { return *reinterpret_cast<const uint16_t*>(rdram + ((address - 0x80000000U) ^ 2U)); };
    auto u32 = [rdram](uint32_t address) { return *reinterpret_cast<const uint32_t*>(rdram + (address - 0x80000000U)); };
    const uint32_t screenWidth = u32(0x8010C430);
    const uint32_t screenHeight = u32(0x8010C434);
    if ((screenWidth == 0) || (screenHeight == 0) || (screenWidth > 1024) || (screenHeight > 1024)) {
        return false;
    }

    const uint32_t left = u16(0x8010C478), top = u16(0x8010C47A), right = u16(0x8010C47C), bottom = u16(0x8010C47E);
    const bool validRect = (right > left) && (bottom > top);
    const bool letterboxed = validRect && ((left > 0) || (top > 0) || (right < screenWidth) || (bottom < screenHeight));
    static bool lastLetterboxed = false;
    static uint64_t lastValues = 0;
    const uint64_t values = (uint64_t(left) << 48) | (uint64_t(top) << 32) | (uint64_t(right) << 16) | bottom;
    if ((letterboxed != lastLetterboxed) || (values != lastValues)) {
        lastLetterboxed = letterboxed;
        lastValues = values;
        printf("[swrs] view rect %u,%u-%u,%u in %ux%u%s\n", left, top, right, bottom, screenWidth, screenHeight, letterboxed ? " (letterboxed)" : "");
    }

    return letterboxed;
}

// F5Rogue reference renderer input (tools\f5ref): saves the task and all of RDRAM right before RT64 runs it, to
// C:\projects\swrs-gfxsnap\task_<n>.bin ("F5SNAP01", the 64-byte OSTask, 8 MB RDRAM in the runtime's layout).
// SWRS_GFX_SNAP_TASKS="n1,n2,..." picks task numbers; or create C:\projects\swrs-gfxsnap-now for the next
// SWRS_GFX_SNAP_COUNT tasks (default 1, checked every 10 tasks). swrs_gfx_snapshot_task is the task being saved
// (0 otherwise) so the renderer can dump its own triangles for the same task.
uint32_t swrs_gfx_snapshot_task = 0;
extern uint32_t f5rogueDebugDumpTask; // RT64 F5Rogue GBI: dumps its triangles for this task (0 = off).
static void swrs_check_gfx_snapshot(const OSTask* task, const uint8_t* rdram, uint32_t dl_count) {
    static std::vector<uint32_t> listed;
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        if (const char* list = getenv("SWRS_GFX_SNAP_TASKS")) {
            for (const char* p = list; *p != '\0';) {
                listed.push_back((uint32_t)strtoul(p, const_cast<char**>(&p), 10));
                while (*p == ',' || *p == ' ') {
                    p++;
                }
            }
        }
    }
    static uint32_t snap_until = 0;
    if ((dl_count % 10) == 0) {
        std::error_code ec;
        if (std::filesystem::remove("C:/projects/swrs-gfxsnap-now", ec)) {
            static const uint32_t count = getenv("SWRS_GFX_SNAP_COUNT") ? (uint32_t)atoi(getenv("SWRS_GFX_SNAP_COUNT")) : 1;
            snap_until = dl_count + count;
        }
    }
    const bool snap = (dl_count < snap_until) || (std::find(listed.begin(), listed.end(), dl_count) != listed.end());
    swrs_gfx_snapshot_task = snap ? dl_count : 0;
    f5rogueDebugDumpTask = swrs_gfx_snapshot_task;
    if (!snap) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories("C:/projects/swrs-gfxsnap", ec);
    char path[128];
    snprintf(path, sizeof(path), "C:/projects/swrs-gfxsnap/task_%u.bin", dl_count);
    FILE* f = fopen(path, "wb");
    if (f == nullptr) {
        return;
    }
    fwrite("F5SNAP01", 1, 8, f);
    fwrite(task, 1, sizeof(OSTask), f);
    fwrite(rdram, 1, 0x800000, f);
    fclose(f);
    printf("[swrs] graphics task %u saved to %s\n", dl_count, path);
}

void zelda64::renderer::RT64Context::send_dl(const OSTask* task) {
    const bool forceOriginal = swrs_force_original_aspect.load() || swrs_view_letterboxed(app->core.RDRAM);
    app->sharedQueueResources->forceOriginalAspectRatio = forceOriginal;
    {
        float scale = 1.0f;
        const uint32_t width = app->sharedQueueResources->swapChainWidth;
        const uint32_t height = app->sharedQueueResources->swapChainHeight;
        if (!forceOriginal && (app->userConfig.aspectRatio == RT64::UserConfiguration::AspectRatio::Expand) && (height > 0)) {
            scale = std::max(1.0f, (float(width) / float(height)) / (4.0f / 3.0f));
        }
        swrs_cull_aspect_scale = scale;
    }
    // Bring-up logging: first few graphics tasks, then every 60th.
    uint32_t dl_count = ++swrs_gfx_task_count;
    if (dl_count <= 10 || dl_count % 60 == 0) {
        printf("[swrs] gfx task %u: ucode 0x%08X data 0x%08X dl 0x%08X size 0x%X\n", dl_count,
            (uint32_t)task->t.ucode, (uint32_t)task->t.ucode_data, (uint32_t)task->t.data_ptr, (uint32_t)task->t.data_size);
    }
    // Bring-up: log RDP state commands for one graphics task (SWRS_RDP_LOG_TASK=<n>).
    static const uint32_t log_task = getenv("SWRS_RDP_LOG_TASK") ? (uint32_t)atoi(getenv("SWRS_RDP_LOG_TASK")) : 0;
    swrs_rdp_log = (dl_count == log_task);
    // Or on demand: create C:\projects\swrs-rdp-log-now to log the next 8 tasks (checked every 10 tasks).
    static uint32_t log_until = 0;
    if (!swrs_rdp_log && (dl_count % 10) == 0) {
        std::error_code ec;
        if (std::filesystem::remove("C:/projects/swrs-rdp-log-now", ec)) {
            static const uint32_t log_count = getenv("SWRS_RDP_LOG_COUNT") ? (uint32_t)atoi(getenv("SWRS_RDP_LOG_COUNT")) : 8;
            log_until = dl_count + log_count;
        }
    }
    if (dl_count < log_until) {
        swrs_rdp_log = 1;
    }
    if (swrs_rdp_log) {
        fprintf(stderr, "[rdp] ---- graphics task %u ----\n", dl_count);
    }
    swrs_check_ram_dump(app->core.RDRAM);
    swrs_check_gfx_snapshot(task, app->core.RDRAM, dl_count);
    check_texture_pack_actions();
    app->state->rsp->reset();
    app->interpreter->loadUCodeGBI(task->t.ucode & 0x3FFFFFF, task->t.ucode_data & 0x3FFFFFF, true);
    app->processDisplayLists(app->core.RDRAM, task->t.data_ptr & 0x3FFFFFF, 0, true);
    swrs_rdp_log = 0;
}

void zelda64::renderer::RT64Context::update_screen() {
    static uint32_t screen_count = 0;
    screen_count++;
    if (screen_count <= 5 || screen_count % 60 == 0) {
        ultramodern::renderer::ViRegs* r = ultramodern::renderer::get_vi_regs();
        printf("[swrs] screen update %u: VI status 0x%X origin 0x%X width %u hstart 0x%X vstart 0x%X xscale 0x%X yscale 0x%X\n",
            screen_count, r->VI_STATUS_REG, r->VI_ORIGIN_REG, r->VI_WIDTH_REG, r->VI_H_START_REG, r->VI_V_START_REG,
            r->VI_X_SCALE_REG, r->VI_Y_SCALE_REG);
    }
    // Bring-up: frame pacing. A new game frame = the VI origin changed. Once a second, print how
    // many VIs and new frames there were and how many VIs each frame stayed on screen.
    {
        static uint32_t last_origin = 0;
        static uint32_t vis_since_change = 0;
        static uint32_t vis = 0, frames = 0;
        static uint32_t hist[6] = {};
        static auto last_report = std::chrono::steady_clock::now();
        static auto last_vi = std::chrono::steady_clock::now();
        static double max_vi_gap_ms = 0;
        const auto now = std::chrono::steady_clock::now();
        max_vi_gap_ms = std::max(max_vi_gap_ms, std::chrono::duration<double, std::milli>(now - last_vi).count());
        last_vi = now;
        const uint32_t origin = ultramodern::renderer::get_vi_regs()->VI_ORIGIN_REG;
        vis++;
        vis_since_change++;
        if (origin != last_origin) {
            hist[std::min(vis_since_change, 5u)]++;
            frames++;
            vis_since_change = 0;
            last_origin = origin;
        }
        if (now - last_report >= std::chrono::seconds(1)) {
            printf("[swrs] frames: %u VIs, %u new frames, frame held 1/2/3/4/5+ VIs: %u/%u/%u/%u/%u, max VI gap %.1f ms\n",
                vis, frames, hist[1], hist[2], hist[3], hist[4], hist[5], max_vi_gap_ms);
            vis = frames = 0;
            for (uint32_t& h : hist) h = 0;
            max_vi_gap_ms = 0;
            last_report = now;
        }
    }
    app->updateScreen();
}

void zelda64::renderer::RT64Context::shutdown() {
    if (app != nullptr) {
        app->end();
    }
}

bool zelda64::renderer::RT64Context::update_config(const ultramodern::renderer::GraphicsConfig& old_config, const ultramodern::renderer::GraphicsConfig& new_config) {
    if (old_config == new_config) {
        return false;
    }

    if (new_config.wm_option != old_config.wm_option) {
        app->setFullScreen(new_config.wm_option == ultramodern::renderer::WindowMode::Fullscreen);
    }

    set_application_user_config(app.get(), new_config);

    app->updateUserConfig(true);

    if (new_config.msaa_option != old_config.msaa_option) {
        app->updateMultisampling();
    }
    return true;
}

void zelda64::renderer::RT64Context::enable_instant_present() {
    // Enable the present early presentation mode for minimal latency.
    app->enhancementConfig.presentation.mode = RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;

    app->updateEnhancementConfig();
}

uint32_t zelda64::renderer::RT64Context::get_display_framerate() const {
    return app->presentQueue->ext.sharedResources->swapChainRate;
}

float zelda64::renderer::RT64Context::get_resolution_scale() const {
    constexpr int ReferenceHeight = 240;
    switch (app->userConfig.resolution) {
        case RT64::UserConfiguration::Resolution::WindowIntegerScale:
            if (app->sharedQueueResources->swapChainHeight > 0) {
                return std::max(float((app->sharedQueueResources->swapChainHeight + ReferenceHeight - 1) / ReferenceHeight), 1.0f);
            }
            else {
                return 1.0f;
            }
        case RT64::UserConfiguration::Resolution::Manual:
            return float(app->userConfig.resolutionMultiplier);
        case RT64::UserConfiguration::Resolution::Original:
        default:
            return 1.0f;
    }
}

void zelda64::renderer::RT64Context::check_texture_pack_actions() {
    bool packs_changed = false;
    TexturePackAction cur_action;
    while (texture_pack_action_queue.try_dequeue(cur_action)) {
        std::visit(overloaded{
            [&](TexturePackDisableAction &to_disable) {
                enabled_texture_packs.erase(to_disable.mod_id);
                packs_changed = true;
            },
            [&](TexturePackEnableAction &to_enable) {
                enabled_texture_packs.insert(to_enable.mod_id);
                packs_changed = true;
            },
            [&](TexturePackSecondaryDisableAction &to_override_disable) {
                secondary_disabled_texture_packs.insert(to_override_disable.mod_id);
                packs_changed = true;
            },
            [&](TexturePackSecondaryEnableAction &to_override_enable) {
                secondary_disabled_texture_packs.erase(to_override_enable.mod_id);
                packs_changed = true;
            },
            [&](TexturePackUpdateAction &) {
                packs_changed = true;
            }
        }, cur_action);
    }

    // If any packs were disabled, unload all packs and load all the active ones.
    if (packs_changed) {
        // Sort the enabled texture packs in reverse order so that earlier ones override later ones.
        std::vector<std::string> sorted_texture_packs{};
        sorted_texture_packs.reserve(enabled_texture_packs.size());
        for (const std::string& mod : enabled_texture_packs) {
            if (!secondary_disabled_texture_packs.contains(mod)) {
                sorted_texture_packs.emplace_back(mod);
            }
        }

        std::sort(sorted_texture_packs.begin(), sorted_texture_packs.end(),
            [](const std::string& lhs, const std::string& rhs) {
                return recomp::mods::get_mod_order_index(lhs) > recomp::mods::get_mod_order_index(rhs);
            }
        );

        // Build the path list from the sorted mod list.
        std::vector<RT64::ReplacementDirectory> replacement_directories;
        replacement_directories.reserve(enabled_texture_packs.size());
        for (const std::string &mod_id : sorted_texture_packs) {
            replacement_directories.emplace_back(RT64::ReplacementDirectory(recomp::mods::get_mod_filename(mod_id)));
        }

        if (!replacement_directories.empty()) {
            app->textureCache->loadReplacementDirectories(replacement_directories);
        }
        else {
            app->textureCache->clearReplacementDirectories();
        }
    }
}

RT64::UserConfiguration::Antialiasing zelda64::renderer::RT64MaxMSAA() {
    return device_max_msaa;
}

std::unique_ptr<ultramodern::renderer::RendererContext> zelda64::renderer::create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    return std::make_unique<zelda64::renderer::RT64Context>(rdram, window_handle, developer_mode);
}

bool zelda64::renderer::RT64SamplePositionsSupported() {
    return sample_positions_supported;
}

bool zelda64::renderer::RT64HighPrecisionFBEnabled() {
    return high_precision_fb_enabled;
}

void zelda64::renderer::trigger_texture_pack_update() {
    texture_pack_action_queue.enqueue(TexturePackUpdateAction{});
}

void zelda64::renderer::enable_texture_pack(const recomp::mods::ModContext& context, const recomp::mods::ModHandle& mod) {
    texture_pack_action_queue.enqueue(TexturePackEnableAction{mod.manifest.mod_id});

    // Check for the texture pack enabled config option.
    const recomp::mods::ConfigSchema& config_schema = context.get_mod_config_schema(mod.manifest.mod_id);
    auto find_it = config_schema.options_by_id.find(zelda64::renderer::special_option_texture_pack_enabled);
    if (find_it != config_schema.options_by_id.end()) {
        const recomp::mods::ConfigOption& config_option = config_schema.options[find_it->second];

        if (is_texture_pack_enable_config_option(config_option, false)) {
            recomp::mods::ConfigValueVariant value_variant = context.get_mod_config_value(mod.manifest.mod_id, config_option.id);
            uint32_t value;
            if (uint32_t* value_ptr = std::get_if<uint32_t>(&value_variant)) {
                value = *value_ptr;
            }
            else {
                value = 0;
            }

            if (value) {
                zelda64::renderer::secondary_enable_texture_pack(mod.manifest.mod_id);
            }
            else {
                zelda64::renderer::secondary_disable_texture_pack(mod.manifest.mod_id);
            }
        }
    }
}

void zelda64::renderer::disable_texture_pack(const recomp::mods::ModHandle& mod) {
    texture_pack_action_queue.enqueue(TexturePackDisableAction{mod.manifest.mod_id});
}

void zelda64::renderer::secondary_enable_texture_pack(const std::string& mod_id) {
    texture_pack_action_queue.enqueue(TexturePackSecondaryEnableAction{mod_id});
}

void zelda64::renderer::secondary_disable_texture_pack(const std::string& mod_id) {
    texture_pack_action_queue.enqueue(TexturePackSecondaryDisableAction{mod_id});
}


// HD texture enable option. Must be an enum with two options.
// The first option is treated as disabled and the second option is treated as enabled.
bool zelda64::renderer::is_texture_pack_enable_config_option(const recomp::mods::ConfigOption& option, bool show_errors) {
    if (option.id == zelda64::renderer::special_option_texture_pack_enabled) {
        if (option.type != recomp::mods::ConfigOptionType::Enum) {
            if (show_errors) {
                recompui::message_box(("Mod has the special config option id for enabling an HD texture pack (\"" + zelda64::renderer::special_option_texture_pack_enabled + "\"), but the config option is not an enum.").c_str());
            }
            return false;
        }

        const recomp::mods::ConfigOptionEnum &option_enum = std::get<recomp::mods::ConfigOptionEnum>(option.variant);
        if (option_enum.options.size() != 2) {
            if (show_errors) {
                recompui::message_box(("Mod has the special config option id for enabling an HD texture pack (\"" + zelda64::renderer::special_option_texture_pack_enabled + "\"), but the config option doesn't have exactly 2 values.").c_str());
            }
            return false;
        }

        return true;
    }
    return false;
}
