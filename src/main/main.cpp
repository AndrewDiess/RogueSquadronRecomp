#include <cstdio>
#include <cassert>
#include <unordered_map>
#include <vector>
#include <array>
#include <filesystem>
#include <numeric>
#include <stdexcept>
#include <cinttypes>

#include "nfd.h"

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#define SDL_MAIN_HANDLED
#ifdef _WIN32
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#include "SDL2/SDL_syswm.h"
// Undefine x11 macros that get included by SDL_syswm.h.
#undef None
#undef Status
#undef LockMask
#undef ControlMask
#undef Success
#undef Always
#endif

#include "recomp_ui.h"
#include "recomp_input.h"
#include "zelda_config.h"
#include "zelda_sound.h"
#include "zelda_render.h"
#include "zelda_support.h"
#include "zelda_game.h"
#include "ovl_patches.hpp"
#include "librecomp/game.hpp"
#include "librecomp/mods.hpp"
#include "librecomp/helpers.hpp"


#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <timeapi.h>
#include "SDL_syswm.h"
#endif

#include "../../lib/rt64/src/contrib/stb/stb_image.h"

const std::string version_string = "1.0.0";

template<typename... Ts>
void exit_error(const char* str, Ts ...args) {
    // TODO pop up an error
    ((void)fprintf(stderr, str, args), ...);
    assert(false);
        
    ultramodern::error_handling::quick_exit(__FILE__, __LINE__, __FUNCTION__);
}

ultramodern::gfx_callbacks_t::gfx_data_t create_gfx() {
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) > 0) {
        exit_error("Failed to initialize SDL2: %s\n", SDL_GetError());
    }

    fprintf(stdout, "SDL Video Driver: %s\n", SDL_GetCurrentVideoDriver());

    return {};
}

#if defined(__gnu_linux__)
#include "icon_bytes.h"

bool SetImageAsIcon(const char* filename, SDL_Window* window)
{
    // Read data
    int width, height, bytesPerPixel;
    void* data = stbi_load_from_memory(reinterpret_cast<const uint8_t*>(icon_bytes), sizeof(icon_bytes), &width, &height, &bytesPerPixel, 4);

    // Calculate pitch
    int pitch;
    pitch = width * 4;
    pitch = (pitch + 3) & ~3;

    // Setup relevance bitmask
    int Rmask, Gmask, Bmask, Amask;

#if SDL_BYTEORDER == SDL_LIL_ENDIAN
    Rmask = 0x000000FF;
    Gmask = 0x0000FF00;
    Bmask = 0x00FF0000;
    Amask = 0xFF000000;
#else
    Rmask = 0xFF000000;
    Gmask = 0x00FF0000;
    Bmask = 0x0000FF00;
    Amask = 0x000000FF;
#endif

    SDL_Surface* surface = nullptr;
    if (data != nullptr) {
        surface = SDL_CreateRGBSurfaceFrom(data, width, height, 32, pitch, Rmask, Gmask,
                            Bmask, Amask);
    }

    if (surface == nullptr) {   
        if (data != nullptr) {
            stbi_image_free(data);
        }
        return false;
	} else {
        SDL_SetWindowIcon(window,surface);
        SDL_FreeSurface(surface);
        stbi_image_free(data);
        return true;
    }
}
#endif

SDL_Window* window;

ultramodern::renderer::WindowHandle create_window(ultramodern::gfx_callbacks_t::gfx_data_t) {
    uint32_t flags = SDL_WINDOW_RESIZABLE;

#if defined(__APPLE__)
    flags |= SDL_WINDOW_METAL;
#elif defined(RT64_SDL_WINDOW_VULKAN)
    flags |= SDL_WINDOW_VULKAN;
#endif

    window = SDL_CreateWindow("Star Wars: Rogue Squadron - Recompiled", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1600, 960,  flags);
#if defined(__linux__)
    SetImageAsIcon("icons/512.png",window);
    if (ultramodern::renderer::get_graphics_config().wm_option == ultramodern::renderer::WindowMode::Fullscreen) { // TODO: Remove once RT64 gets native fullscreen support on Linux
        SDL_SetWindowFullscreen(window,SDL_WINDOW_FULLSCREEN_DESKTOP);
    } else {
        SDL_SetWindowFullscreen(window,0);
    }
#endif

    if (window == nullptr) {
        exit_error("Failed to create window: %s\n", SDL_GetError());
    }

    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    SDL_GetWindowWMInfo(window, &wmInfo);

#if defined(_WIN32)
    return ultramodern::renderer::WindowHandle{ wmInfo.info.win.window, GetCurrentThreadId() };
#elif defined(__linux__) || defined(__ANDROID__)
    return ultramodern::renderer::WindowHandle{ window };
#elif defined(__APPLE__)
    SDL_MetalView view = SDL_Metal_CreateView(window);
    return ultramodern::renderer::WindowHandle{ wmInfo.info.cocoa.window,  SDL_Metal_GetLayer(view) };
#else
    static_assert(false && "Unimplemented");
#endif
}

void update_gfx(void*) {
    recomp::handle_events();
}

static SDL_AudioCVT audio_convert;
static SDL_AudioDeviceID audio_device = 0;

// Samples per channel per second.
static uint32_t sample_rate = 48000;
static uint32_t output_sample_rate = 48000;
// Channel count.
constexpr uint32_t input_channels = 2;
static uint32_t output_channels = 2;

// Terminology: a frame is a collection of samples for each channel. e.g. 2 input samples is one input frame. This is unrelated to graphical frames.

// Number of frames to duplicate for fixing interpolation at the start and end of a chunk.
constexpr uint32_t duplicated_input_frames = 4;
// The number of output frames to skip for playback (to avoid playing duplicate inputs twice).
static uint32_t discarded_output_frames;

constexpr uint32_t bytes_per_frame = input_channels * sizeof(float);

uint32_t swrs_audio_skips = 0;
namespace swrs {
    void wait_audio_buffer_ready(const int16_t* samples);
    extern std::atomic<uint32_t> audio_buffer_waits;
    extern std::atomic<uint32_t> audio_buffer_wait_timeouts;
}

void queue_samples(int16_t* audio_data, size_t sample_count) {
    swrs::wait_audio_buffer_ready(audio_data);
    // Bring-up: once a second, report how much audio arrives and its peak level.
    {
        static auto last_report = std::chrono::steady_clock::now();
        static uint32_t buffers = 0;
        static size_t samples = 0;
        static int peak = 0;
        static double sum_sq = 0.0;
        static size_t clipped = 0;
        static size_t sign_changes = 0;
        static int prev = 0;
        static uint32_t underruns = 0;
        if (SDL_GetQueuedAudioSize(audio_device) == 0) {
            underruns++; // playback ran dry before this buffer arrived
        }
        buffers++;
        samples += sample_count;
        for (size_t i = 0; i < sample_count; i++) {
            const int v = audio_data[i];
            peak = std::max(peak, std::abs(v));
            sum_sq += double(v) * v;
            clipped += (std::abs(v) >= 32000);
            if ((i & 1) == 0) { // one channel
                sign_changes += ((v < 0) != (prev < 0));
                prev = v;
            }
        }
        auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(1)) {
            printf("[swrs] audio: %u buffers, %zu samples, rate %u Hz, peak %d, rms %.0f, clipped %.2f%%, sign changes/s %zu, underruns %u, skips %u, late buffer waits %u (timeouts %u)\n",
                buffers, samples, sample_rate, peak, samples ? std::sqrt(sum_sq / samples) : 0.0,
                samples ? 100.0 * clipped / samples : 0.0, sign_changes, underruns, swrs_audio_skips,
                swrs::audio_buffer_waits.exchange(0), swrs::audio_buffer_wait_timeouts.exchange(0));
            underruns = 0;
            swrs_audio_skips = 0;
            buffers = 0;
            samples = 0;
            peak = 0;
            sum_sq = 0.0;
            clipped = 0;
            sign_changes = 0;
            last_report = now;
        }
    }

    // Buffer for holding the output of swapping the audio channels. This is reused across
    // calls to reduce runtime allocations.
    static std::vector<float> swap_buffer;
    static std::array<float, duplicated_input_frames * input_channels> duplicated_sample_buffer;

    // Make sure the swap buffer is large enough to hold the audio data, including any extra space needed for resampling.
    size_t resampled_sample_count = sample_count + duplicated_input_frames * input_channels;
    size_t max_sample_count = std::max(resampled_sample_count, resampled_sample_count * audio_convert.len_mult);
    if (max_sample_count > swap_buffer.size()) {
        swap_buffer.resize(max_sample_count);
    }
    
    // Copy the duplicated frames from last chunk into this chunk
    for (size_t i = 0; i < duplicated_input_frames * input_channels; i++) {
        swap_buffer[i] = duplicated_sample_buffer[i];
    }

    // Convert the audio from 16-bit values to floats and swap the audio channels into the
    // swap buffer to correct for the address xor caused by endianness handling.
    float cur_main_volume = zelda64::get_main_volume() / 100.0f; // Get the current main volume, normalized to 0.0-1.0.
    for (size_t i = 0; i < sample_count; i += input_channels) {
        swap_buffer[i + 0 + duplicated_input_frames * input_channels] = audio_data[i + 1] * (0.5f / 32768.0f) * cur_main_volume;
        swap_buffer[i + 1 + duplicated_input_frames * input_channels] = audio_data[i + 0] * (0.5f / 32768.0f) * cur_main_volume;
    }
    
    // TODO handle cases where a chunk is smaller than the duplicated frame count.
    assert(sample_count > duplicated_input_frames * input_channels);

    // Copy the last converted samples into the duplicated sample buffer to reuse in resampling the next queued chunk.
    for (size_t i = 0; i < duplicated_input_frames * input_channels; i++) {
        duplicated_sample_buffer[i] = swap_buffer[i + sample_count];
    }
    
    audio_convert.buf = reinterpret_cast<Uint8*>(swap_buffer.data());
    audio_convert.len = (sample_count + duplicated_input_frames * input_channels) * sizeof(swap_buffer[0]);

    int ret = SDL_ConvertAudio(&audio_convert);

    if (ret < 0) {
        printf("Error using SDL audio converter: %s\n", SDL_GetError());
        throw std::runtime_error("Error using SDL audio converter");
    }

    uint64_t cur_queued_microseconds = uint64_t(SDL_GetQueuedAudioSize(audio_device)) / bytes_per_frame * 1000000 / sample_rate;
    uint32_t num_bytes_to_queue = audio_convert.len_cvt - output_channels * discarded_output_frames * sizeof(swap_buffer[0]);
    float* samples_to_queue = swap_buffer.data() + output_channels * discarded_output_frames / 2;

    // Prevent audio latency from building up by skipping samples in incoming audio when too many samples are already queued.
    // Skip samples based on how many microseconds of samples are queued already.
    uint32_t skip_factor = cur_queued_microseconds / 100000;
    if (skip_factor != 0) {
        swrs_audio_skips++;
        uint32_t skip_ratio = 1 << skip_factor;
        num_bytes_to_queue /= skip_ratio;
        for (size_t i = 0; i < num_bytes_to_queue / (output_channels * sizeof(swap_buffer[0])); i++) {
            samples_to_queue[2 * i + 0] = samples_to_queue[2 * skip_ratio * i + 0];
            samples_to_queue[2 * i + 1] = samples_to_queue[2 * skip_ratio * i + 1];
        }
    }

    // Queue the swapped audio data.
    // Offset the data start by only half the discarded frame count as the other half of the discarded frames are at the end of the buffer.
    SDL_QueueAudio(audio_device, samples_to_queue, num_bytes_to_queue);

    // Bring-up: with SWRS_AUDIO_DUMP set, write the game's samples (s16 stereo, as received)
    // and the queued output (f32 stereo, output rate) to raw files in the working directory.
    static FILE* dump_in = nullptr;
    static FILE* dump_out = nullptr;
    static bool dump_checked = false;
    if (!dump_checked) {
        dump_checked = true;
        if (std::getenv("SWRS_AUDIO_DUMP")) {
            dump_in = std::fopen("swrs-audio-in.raw", "wb");
            dump_out = std::fopen("swrs-audio-out.raw", "wb");
        }
    }
    if (dump_in) {
        // Per buffer: u32 rate, u32 sample count, then the samples (keeps the chunk edges).
        uint32_t header[2] = { sample_rate, (uint32_t)sample_count };
        std::fwrite(header, sizeof(header), 1, dump_in);
        std::fwrite(audio_data, sizeof(int16_t), sample_count, dump_in);
        std::fwrite(&num_bytes_to_queue, sizeof(num_bytes_to_queue), 1, dump_out);
        std::fwrite(samples_to_queue, 1, num_bytes_to_queue, dump_out);
    }
}

size_t get_frames_remaining() {
    constexpr float buffer_offset_frames = 1.0f;
    // Get the number of remaining buffered audio bytes.
    uint64_t buffered_byte_count = SDL_GetQueuedAudioSize(audio_device);

    // Scale the byte count based on the ratio of sample rates and channel counts.
    buffered_byte_count = buffered_byte_count * 2 * sample_rate / output_sample_rate / output_channels;

    // Adjust the reported count to be some number of refreshes in the future, which helps ensure that
    // there are enough samples even if the audio thread experiences a small amount of lag. This prevents
    // audio popping on games that use the buffered audio byte count to determine how many samples
    // to generate.
    uint32_t frames_per_vi = (sample_rate / 60);
    if (buffered_byte_count > (buffer_offset_frames * bytes_per_frame * frames_per_vi)) {
        buffered_byte_count -= (buffer_offset_frames * bytes_per_frame * frames_per_vi);
    }
    else {
        buffered_byte_count = 0;
    }
    // Convert from byte count to sample count.
    return static_cast<uint32_t>(buffered_byte_count / bytes_per_frame);
}

void update_audio_converter() {
    int ret = SDL_BuildAudioCVT(&audio_convert, AUDIO_F32, input_channels, sample_rate, AUDIO_F32, output_channels, output_sample_rate);

    if (ret < 0) {
        printf("Error creating SDL audio converter: %s\n", SDL_GetError());
        throw std::runtime_error("Error creating SDL audio converter");
    }

    // Calculate the number of samples to discard based on the sample rate ratio and the duplicate frame count.
    discarded_output_frames = duplicated_input_frames * output_sample_rate / sample_rate;
}

void set_frequency(uint32_t freq) {
    sample_rate = freq;
    
    update_audio_converter();
}

void reset_audio(uint32_t output_freq) {
    SDL_AudioSpec spec_desired{
        .freq = (int)output_freq,
        .format = AUDIO_F32,
        .channels = (Uint8)output_channels,
        .silence = 0, // calculated
        .samples = 0x100, // Fairly small sample count to reduce the latency of internal buffering
        .padding = 0, // unused
        .size = 0, // calculated
        .callback = nullptr,
        .userdata = nullptr
    };


    audio_device = SDL_OpenAudioDevice(nullptr, false, &spec_desired, nullptr, 0);
    if (audio_device == 0) {
        exit_error("SDL error opening audio device: %s\n", SDL_GetError());
    }
    SDL_PauseAudioDevice(audio_device, 0);

    output_sample_rate = output_freq;
    update_audio_converter();
}

namespace swrs {
    RspUcodeFunc* get_rsp_microcode(const OSTask* task);
    std::string get_game_thread_name(const OSThread* t);
    extern std::atomic<uint32_t> audio_task_count;
}

// Bring-up debug counters (rt64_render_context.cpp, ultramodern threads.cpp/mesgqueue.cpp).
extern std::atomic<uint32_t> swrs_gfx_task_count;
namespace ultramodern {
    extern std::atomic<OSThread*> swrs_debug_thread[64];
    extern std::atomic<uint32_t> swrs_debug_thread_vram[64];
    extern std::atomic<uint32_t> swrs_debug_thread_entry[64];
    extern std::atomic<uint32_t> swrs_debug_blocked_on[64];
    extern std::atomic<int> swrs_debug_blocked_kind[64];
    extern std::atomic<int> swrs_debug_thread_count;
}

extern "C" void recomp_entrypoint(uint8_t * rdram, recomp_context * ctx);
gpr get_entrypoint_address();

// array of supported GameEntry objects
std::vector<recomp::GameEntry> supported_games = {
    {
        // XXH3-64 of the US V1.1 ROM (SHA-1 c12e1c6fb47a67da463c50b705ec682ec24ae80d).
        .rom_hash = 0xE1C301FBE06D6F73ULL,
        .internal_name = "rogue squadron",
        .game_id = u8"swrs.n64.us.1.1",
        .mod_game_id = "swrs",
        // The game only checks that an EEPROM exists; its size isn't confirmed yet.
        .save_type = recomp::SaveType::AllowAll,
        .is_enabled = true,
        .has_compressed_code = false,
        .entrypoint_address = get_entrypoint_address(),
        .entrypoint = recomp_entrypoint,
    },
};

#ifdef _WIN32

struct PreloadContext {
    HANDLE handle;
    HANDLE mapping_handle;
    SIZE_T size;
    PVOID view;
};

bool preload_executable(PreloadContext& context) {
    wchar_t module_name[MAX_PATH];
    GetModuleFileNameW(NULL, module_name, MAX_PATH);

    context.handle = CreateFileW(module_name, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (context.handle == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Failed to load executable into memory!");
        context = {};
        return false;
    }

    LARGE_INTEGER module_size;
    if (!GetFileSizeEx(context.handle, &module_size)) {
        fprintf(stderr, "Failed to get size of executable!");
        CloseHandle(context.handle);
        context = {};
        return false;
    }

    context.size = module_size.QuadPart;

    context.mapping_handle = CreateFileMappingW(context.handle, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (context.mapping_handle == nullptr) {
        fprintf(stderr, "Failed to create file mapping of executable!");
        CloseHandle(context.handle);
        context = {};
        return EXIT_FAILURE;
    }

    context.view = MapViewOfFile(context.mapping_handle, FILE_MAP_READ, 0, 0, 0);
    if (context.view == nullptr) {
        fprintf(stderr, "Failed to map view of of executable!");
        CloseHandle(context.mapping_handle);
        CloseHandle(context.handle);
        context = {};
        return false;
    }

    DWORD pid = GetCurrentProcessId();
    HANDLE process_handle = OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (process_handle == nullptr) {
        fprintf(stderr, "Failed to open own process!");
        CloseHandle(context.mapping_handle);
        CloseHandle(context.handle);
        context = {};
        return false;
    }

    SIZE_T minimum_set_size, maximum_set_size;
    if (!GetProcessWorkingSetSize(process_handle, &minimum_set_size, &maximum_set_size)) {
        fprintf(stderr, "Failed to get working set size!");
        CloseHandle(context.mapping_handle);
        CloseHandle(context.handle);
        context = {};
        return false;
    }

    if (!SetProcessWorkingSetSize(process_handle, minimum_set_size + context.size, maximum_set_size + context.size)) {
        fprintf(stderr, "Failed to set working set size!");
        CloseHandle(context.mapping_handle);
        CloseHandle(context.handle);
        context = {};
        return false;
    }

    if (VirtualLock(context.view, context.size) == 0) {
        fprintf(stderr, "Failed to lock view of executable! (Error: %08lx)\n", GetLastError());
        CloseHandle(context.mapping_handle);
        CloseHandle(context.handle);
        context = {};
        return false;
    }
    
    return true;
}

void release_preload(PreloadContext& context) {
    VirtualUnlock(context.view, context.size);
    CloseHandle(context.mapping_handle);
    CloseHandle(context.handle);
    context = {};
}

#else

struct PreloadContext {

};

// TODO implement on other platforms
bool preload_executable(PreloadContext& context) {
    return false;
}

void release_preload(PreloadContext& context) {
}

#endif

void enable_texture_pack(recomp::mods::ModContext& context, const recomp::mods::ModHandle& mod) {
    zelda64::renderer::enable_texture_pack(context, mod);
}

void disable_texture_pack(recomp::mods::ModContext&, const recomp::mods::ModHandle& mod) {
    zelda64::renderer::disable_texture_pack(mod);
}

void reorder_texture_pack(recomp::mods::ModContext&) {
    zelda64::renderer::trigger_texture_pack_update();
}

int main(int argc, char** argv) {
    // Bring-up: unbuffered output so logs survive crashes and forced exits.
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    // --start: skip the launcher and start the stored ROM right away (for testing).
    bool autostart = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--start") == 0) {
            autostart = true;
        }
    }

    recomp::Version project_version{};
    if (!recomp::Version::from_string(version_string, project_version)) {
        ultramodern::error_handling::message_box(("Invalid version string: " + version_string).c_str());
        return EXIT_FAILURE;
    }

    // Map this executable into memory and lock it, which should keep it in physical memory. This ensures
    // that there are no stutters from the OS having to load new pages of the executable whenever a new code page is run.
    PreloadContext preload_context;
    bool preloaded = preload_executable(preload_context);

    if (!preloaded) {
        fprintf(stderr, "Failed to preload executable!\n");
    }

#ifdef _WIN32
    // Set up high resolution timing period.
    timeBeginPeriod(1);

    // Process arguments.
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--show-console") == 0)
        {
            if (GetConsoleWindow() == nullptr)
            {
                AllocConsole();
                freopen("CONIN$", "r", stdin);
                freopen("CONOUT$", "w", stderr);
                freopen("CONOUT$", "w", stdout);
            }

            break;
        }
    }

    // Set up console output to accept UTF-8 on windows
    SetConsoleOutputCP(CP_UTF8);

    // Initialize native file dialogs.
    NFD_Init();

    // Change to a font that supports Japanese characters
    CONSOLE_FONT_INFOEX cfi;
    cfi.cbSize = sizeof cfi;
    cfi.nFont = 0;
    cfi.dwFontSize.X = 0;
    cfi.dwFontSize.Y = 16;
    cfi.FontFamily = FF_DONTCARE;
    cfi.FontWeight = FW_NORMAL;
    wcscpy_s(cfi.FaceName, L"NSimSun");
    SetCurrentConsoleFontEx(GetStdHandle(STD_OUTPUT_HANDLE), FALSE, &cfi);
#endif

#ifdef _WIN32
    // Force wasapi on Windows, as there seems to be some issue with sample queueing with directsound currently.
    SDL_setenv("SDL_AUDIODRIVER", "wasapi", true);
#endif

#if defined(__linux__) && defined(RECOMP_FLATPAK)
    // When using Flatpak, applications tend to launch from the home directory by default.
    // Mods might use the current working directory to store the data, so we switch it to a directory
    // with persistent data storage and write permissions under Flatpak to ensure it works.
    std::error_code ec;
    std::filesystem::current_path("/var/data", ec);
#endif

    // Initialize SDL audio and set the output frequency.
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    reset_audio(48000);

    // Source controller mappings file
    std::u8string controller_db_path = (zelda64::get_program_path() / "recompcontrollerdb.txt").u8string();
    if (SDL_GameControllerAddMappingsFromFile(reinterpret_cast<const char *>(controller_db_path.c_str())) < 0) {
        fprintf(stderr, "Failed to load controller mappings: %s\n", SDL_GetError());
    }

    recomp::register_config_path(zelda64::get_app_folder_path());

    // Register supported games and patches
    for (const auto& game : supported_games) {
        recomp::register_game(game);
    }

    recompui::register_ui_exports();

    zelda64::register_overlays();
    zelda64::load_config();

    recomp::rsp::callbacks_t rsp_callbacks{
        .get_rsp_microcode = swrs::get_rsp_microcode,
    };

    ultramodern::renderer::callbacks_t renderer_callbacks{
        .create_render_context = zelda64::renderer::create_render_context,
    };

    ultramodern::gfx_callbacks_t gfx_callbacks{
        .create_gfx = create_gfx,
        .create_window = create_window,
        .update_gfx = update_gfx,
    };

    ultramodern::audio_callbacks_t audio_callbacks{
        .queue_samples = queue_samples,
        .get_frames_remaining = get_frames_remaining,
        .set_frequency = set_frequency,
    };

    ultramodern::input::callbacks_t input_callbacks{
        .poll_input = recomp::poll_inputs,
        .get_input = recomp::get_n64_input,
        .set_rumble = recomp::set_rumble,
        .get_connected_device_info = recomp::get_connected_device_info,
    };

    ultramodern::events::callbacks_t thread_callbacks{
        .vi_callback = recomp::update_rumble,
        .gfx_init_callback = recompui::update_supported_options,
    };

    ultramodern::error_handling::callbacks_t error_handling_callbacks{
        .message_box = recompui::message_box,
    };

    ultramodern::threads::callbacks_t threads_callbacks{
        .get_game_thread_name = swrs::get_game_thread_name,
    };

    // Register the texture pack content type with rt64.json as its content file.
    recomp::mods::ModContentType texture_pack_content_type{
        .content_filename = "rt64.json",
        .allow_runtime_toggle = true,
        .on_enabled = enable_texture_pack,
        .on_disabled = disable_texture_pack,
        .on_reordered = reorder_texture_pack,
    };
    auto texture_pack_content_type_id = recomp::mods::register_mod_content_type(texture_pack_content_type);

    // Register the .rtz texture pack file format with the previous content type as its only allowed content type.
    recomp::mods::register_mod_container_type("rtz", std::vector{ texture_pack_content_type_id }, false);

    if (autostart) {
        // Wait until the runtime is up: the VI thread installs a dummy VI mode only while
        // the game isn't started, and dereferences it on its first tick, so starting the
        // game before that crashes in update_vi. The launcher stays visible (logs only).
        std::thread([]() {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            printf("[swrs] --start: starting the stored ROM\n");
            recomp::start_game(supported_games[0].game_id);
            recompui::hide_all_contexts(); // same as the launcher's Start button (takes the UI lock)

            // Bring-up monitor: task counts and which threads wait on which queue.
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(3));
                printf("[swrs] monitor: gfx tasks %u, audio tasks %u\n",
                    swrs_gfx_task_count.load(), swrs::audio_task_count.load());
                static const char* state_names[] = { "stopped", "queued", "running", "blocked" };
                int n = ultramodern::swrs_debug_thread_count.load();
                for (int i = 0; i < n; i++) {
                    OSThread* t = ultramodern::swrs_debug_thread[i].load();
                    if (t == nullptr) {
                        continue; // thread has ended
                    }
                    int kind = ultramodern::swrs_debug_blocked_kind[i].load();
                    uint32_t q = ultramodern::swrs_debug_blocked_on[i].load();
                    printf("[swrs]   thread 0x%08X entry 0x%08X id %d pri %d: %s%s",
                        ultramodern::swrs_debug_thread_vram[i].load(), ultramodern::swrs_debug_thread_entry[i].load(),
                        (int)t->id, (int)t->priority, t->state < 4 ? state_names[t->state] : "?",
                        kind ? (kind == 1 ? ", waiting to receive on " : ", waiting to send to ") : "");
                    if (kind) {
                        printf("0x%08X", q);
                    }
#ifdef _WIN32
                    // Not waiting on a queue: sample where its host thread is executing.
                    // Offsets are into this exe (symbolize with llvm-symbolizer 0x140000000+off).
                    if (!kind && t->context != nullptr) {
                        HANDLE h = (HANDLE)t->context->host_thread.native_handle();
                        uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
                        printf(", pc samples:");
                        for (int s = 0; s < 3; s++) {
                            CONTEXT c{};
                            c.ContextFlags = CONTEXT_CONTROL;
                            if (SuspendThread(h) != (DWORD)-1) {
                                if (GetThreadContext(h, &c)) {
                                    uintptr_t rip = c.Rip;
                                    if (rip >= base && rip < base + 0x10000000) {
                                        printf(" exe+0x%llX", (unsigned long long)(rip - base));
                                    } else {
                                        printf(" outside");
                                    }
                                }
                                ResumeThread(h);
                            }
                            std::this_thread::sleep_for(std::chrono::milliseconds(7));
                        }
                    }
#endif
                    printf("\n");
                }
            }
        }).detach();
    }

    recomp::start(
        project_version,
        {},
        rsp_callbacks,
        renderer_callbacks,
        audio_callbacks,
        input_callbacks,
        gfx_callbacks,
        thread_callbacks,
        error_handling_callbacks,
        threads_callbacks
    );

    NFD_Quit();

    if (preloaded) {
        release_preload(preload_context);
    }

#ifdef _WIN32
    // End high resolution timing period.
    timeEndPeriod(1);
#endif

    return EXIT_SUCCESS;
}
