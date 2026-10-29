/**
 * @file src/platform/windows/clipboard.cpp
 * @brief Definitions for sharing the host clipboard on Windows.
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

// platform includes
#include <objidl.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <windows.h>

// local includes
#include "src/clipboard.h"
#include "src/logging.h"
#include "src/utility.h"
#include "utf_utils.h"

using namespace std::literals;

namespace clipboard {
  namespace {
    /// CLSID_WICImagingFactory, spelled out so nothing depends on which import library defines it.
    constexpr GUID wic_imaging_factory {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
    /// IID_IWICImagingFactory.
    constexpr GUID wic_imaging_factory_interface {0xec5ec8a9, 0xc395, 0x4314, {0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70}};
    /// GUID_ContainerFormatPng.
    constexpr GUID png_container {0x1b7cfaf4, 0x713f, 0x473c, {0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf}};
    /// GUID_WICPixelFormat32bppBGRA.
    constexpr GUID bgra_pixels {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0f}};

    /// How long a caller waits for the clipboard thread to answer.
    constexpr auto answer_within {3s};

    /// A bitmap on the clipboard may be much larger than the PNG made from it.
    constexpr std::size_t max_bitmap_bytes {4 * max_content_bytes};

    /**
     * @brief Releases a COM interface when it goes out of scope.
     */
    template<class T>
    struct com_ptr_t {
      T *p {nullptr};

      com_ptr_t() = default;
      com_ptr_t(const com_ptr_t &) = delete;
      com_ptr_t &operator=(const com_ptr_t &) = delete;

      ~com_ptr_t() {
        if (p) {
          p->Release();
        }
      }

      T **out() {
        return &p;
      }

      T *operator->() const {
        return p;
      }
    };

    /**
     * @brief Copy what a clipboard handle holds.
     *
     * @param format The format to copy.
     * @param limit The most bytes to accept.
     * @return The bytes, or nothing if the clipboard has none or too many.
     */
    std::optional<std::string> copy_format(UINT format, std::size_t limit) {
      const auto handle {GetClipboardData(format)};
      if (!handle) {
        return std::nullopt;
      }

      const auto size {GlobalSize(handle)};
      if (size == 0 || size > limit) {
        return std::nullopt;
      }

      const auto *bytes {static_cast<const char *>(GlobalLock(handle))};
      if (!bytes) {
        return std::nullopt;
      }
      std::string copy {bytes, size};
      GlobalUnlock(handle);
      return copy;
    }

    /**
     * @brief Move bytes into memory the clipboard can take over.
     *
     * @param bytes What to put there.
     * @return The handle, or nothing if it could not be allocated.
     */
    HGLOBAL global_copy(std::string_view bytes) {
      auto handle {GlobalAlloc(GMEM_MOVEABLE, bytes.size())};
      if (!handle) {
        return nullptr;
      }

      auto *target {GlobalLock(handle)};
      if (!target) {
        GlobalFree(handle);
        return nullptr;
      }
      std::memcpy(target, bytes.data(), bytes.size());
      GlobalUnlock(handle);
      return handle;
    }

    /**
     * @brief Decode an image and hand back its pixels as top-down BGRA rows.
     *
     * @param image An image file in a format Windows can decode.
     * @param width Receives the width.
     * @param height Receives the height.
     * @return The pixels, or nothing if the image could not be decoded.
     */
    std::optional<std::string> decode_bgra(std::string_view image, UINT &width, UINT &height) {
      com_ptr_t<IWICImagingFactory> factory;
      if (FAILED(CoCreateInstance(wic_imaging_factory, nullptr, CLSCTX_INPROC_SERVER, wic_imaging_factory_interface, reinterpret_cast<void **>(factory.out())))) {
        return std::nullopt;
      }

      com_ptr_t<IStream> stream;
      stream.p = SHCreateMemStream(reinterpret_cast<const BYTE *>(image.data()), static_cast<UINT>(image.size()));
      if (!stream.p) {
        return std::nullopt;
      }

      com_ptr_t<IWICBitmapDecoder> decoder;
      com_ptr_t<IWICBitmapFrameDecode> frame;
      com_ptr_t<IWICFormatConverter> converter;
      if (FAILED(factory->CreateDecoderFromStream(stream.p, nullptr, WICDecodeMetadataCacheOnDemand, decoder.out())) || FAILED(decoder->GetFrame(0, frame.out())) || FAILED(factory->CreateFormatConverter(converter.out())) || FAILED(converter->Initialize(frame.p, bgra_pixels, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) || FAILED(converter->GetSize(&width, &height))) {
        return std::nullopt;
      }

      const std::uint64_t stride {std::uint64_t {width} * 4};
      if (width == 0 || height == 0 || stride * height > max_bitmap_bytes) {
        return std::nullopt;
      }

      std::string pixels(stride * height, '\0');
      if (FAILED(converter->CopyPixels(nullptr, static_cast<UINT>(stride), static_cast<UINT>(pixels.size()), reinterpret_cast<BYTE *>(pixels.data())))) {
        return std::nullopt;
      }
      return pixels;
    }

    /**
     * @brief Re-encode an image as a PNG file.
     *
     * @param image An image file in a format Windows can decode.
     * @return The PNG file, or nothing if the image could not be converted.
     */
    std::optional<std::string> encode_png(std::string_view image) {
      UINT width {};
      UINT height {};
      const auto pixels {decode_bgra(image, width, height)};
      if (!pixels) {
        return std::nullopt;
      }

      com_ptr_t<IWICImagingFactory> factory;
      if (FAILED(CoCreateInstance(wic_imaging_factory, nullptr, CLSCTX_INPROC_SERVER, wic_imaging_factory_interface, reinterpret_cast<void **>(factory.out())))) {
        return std::nullopt;
      }

      com_ptr_t<IStream> stream;
      if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.out()))) {
        return std::nullopt;
      }

      com_ptr_t<IWICBitmapEncoder> encoder;
      com_ptr_t<IWICBitmapFrameEncode> frame;
      com_ptr_t<IPropertyBag2> options;
      WICPixelFormatGUID format {bgra_pixels};
      if (FAILED(factory->CreateEncoder(png_container, nullptr, encoder.out())) || FAILED(encoder->Initialize(stream.p, WICBitmapEncoderNoCache)) || FAILED(encoder->CreateNewFrame(frame.out(), options.out())) || FAILED(frame->Initialize(options.p)) || FAILED(frame->SetSize(width, height)) || FAILED(frame->SetPixelFormat(&format)) || format != bgra_pixels || FAILED(frame->WritePixels(height, width * 4, static_cast<UINT>(pixels->size()), reinterpret_cast<BYTE *>(const_cast<char *>(pixels->data())))) || FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
        return std::nullopt;
      }

      // The stream's memory can be larger than what was written to it
      ULARGE_INTEGER written {};
      if (FAILED(stream->Seek({}, STREAM_SEEK_CUR, &written)) || written.QuadPart > max_content_bytes) {
        return std::nullopt;
      }

      HGLOBAL memory {};
      if (FAILED(GetHGlobalFromStream(stream.p, &memory))) {
        return std::nullopt;
      }
      const auto *bytes {static_cast<const char *>(GlobalLock(memory))};
      if (!bytes) {
        return std::nullopt;
      }
      std::string png {bytes, static_cast<std::size_t>(written.QuadPart)};
      GlobalUnlock(memory);
      return png;
    }

    /**
     * @brief Owns the clipboard for Sunshine.
     *
     * Writing the clipboard takes a window, and whoever owns the clipboard is
     * sent messages when others use it, so the window lives on a thread of
     * its own that keeps answering them. Everything clipboard related runs on
     * that thread, which also keeps COM, which the image conversions need,
     * set up in one place.
     */
    class clipboard_thread_t {
    public:
      /**
       * @brief The one clipboard thread, started on first use.
       */
      static clipboard_thread_t &instance() {
        static clipboard_thread_t thread;
        return thread;
      }

      /**
       * @brief Run work on the clipboard thread and wait for its answer.
       *
       * @param work What to run.
       * @return Its answer, or nothing if the thread is not running or did not answer in time.
       */
      template<class T>
      std::optional<T> run(std::function<std::optional<T>(HWND)> work) {
        if (!m_thread_id) {
          return std::nullopt;
        }

        auto task {std::make_shared<std::packaged_task<std::optional<T>()>>([work = std::move(work), window = m_window]() {
          return work(window);
        })};
        auto answer {task->get_future()};
        {
          std::lock_guard lock {m_mutex};
          m_tasks.emplace_back([task]() {
            (*task)();
          });
        }
        PostThreadMessageW(m_thread_id, WM_APP, 0, 0);

        if (answer.wait_for(answer_within) != std::future_status::ready) {
          BOOST_LOG(warning) << "The clipboard thread did not answer in time"sv;
          return std::nullopt;
        }
        return answer.get();
      }

      clipboard_thread_t(const clipboard_thread_t &) = delete;
      clipboard_thread_t &operator=(const clipboard_thread_t &) = delete;

    private:
      clipboard_thread_t() {
        std::promise<void> ready;
        auto started {ready.get_future()};
        std::thread {[this, &ready]() {
          loop(ready);
        }}.detach();
        started.wait();
      }

      void loop(std::promise<void> &ready) {
        // Makes the message queue exist before anyone posts to it
        MSG message;
        PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

        const auto com {CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)};

        WNDCLASSEXW window_class {};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = DefWindowProcW;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.lpszClassName = L"SunshineClipboard";
        RegisterClassExW(&window_class);
        m_window = CreateWindowExW(0, window_class.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, window_class.hInstance, nullptr);

        if (m_window && SUCCEEDED(com)) {
          m_thread_id = GetCurrentThreadId();
        } else {
          BOOST_LOG(error) << "Could not set up the clipboard thread, so the clipboard is not shared"sv;
        }
        ready.set_value();
        if (!m_thread_id) {
          return;
        }

        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
          if (!message.hwnd && message.message == WM_APP) {
            std::deque<std::function<void()>> tasks;
            {
              std::lock_guard lock {m_mutex};
              tasks.swap(m_tasks);
            }
            for (auto &task : tasks) {
              task();
            }
            continue;
          }

          TranslateMessage(&message);
          DispatchMessageW(&message);
        }
      }

      std::mutex m_mutex;
      std::deque<std::function<void()>> m_tasks;
      std::atomic<DWORD> m_thread_id {0};
      HWND m_window {nullptr};
    };

    /**
     * @brief Open the clipboard, waiting briefly for whoever has it open.
     *
     * @param window The window that will own what is written.
     * @return True once it is open.
     */
    bool open_clipboard(HWND window) {
      for (int attempt = 0; attempt < 10; ++attempt) {
        if (OpenClipboard(window)) {
          return true;
        }
        std::this_thread::sleep_for(20ms);
      }
      BOOST_LOG(warning) << "The clipboard is held open by another program"sv;
      return false;
    }

    /**
     * @brief The format some programs put PNG files on the clipboard as.
     */
    UINT png_format() {
      static const UINT format {RegisterClipboardFormatW(L"PNG")};
      return format;
    }

    std::optional<content_t> read_clipboard(HWND window) {
      if (!open_clipboard(window)) {
        return std::nullopt;
      }

      std::optional<std::string> text;
      std::optional<std::string> png;
      std::optional<std::string> bitmap;
      {
        auto close {util::fail_guard([]() {
          CloseClipboard();
        })};

        if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
          text = copy_format(CF_UNICODETEXT, 2 * max_content_bytes);
        } else if (png_format() && IsClipboardFormatAvailable(png_format())) {
          png = copy_format(png_format(), max_content_bytes);
        } else if (IsClipboardFormatAvailable(CF_DIBV5)) {
          bitmap = copy_format(CF_DIBV5, max_bitmap_bytes);
        } else if (IsClipboardFormatAvailable(CF_DIB)) {
          bitmap = copy_format(CF_DIB, max_bitmap_bytes);
        }
      }

      // Converted once the clipboard is closed, so others are not kept waiting
      if (text) {
        std::wstring wide {reinterpret_cast<const wchar_t *>(text->data()), text->size() / sizeof(wchar_t)};
        wide.resize(std::wcslen(wide.c_str()));
        auto utf8 {utf_utils::to_utf8(wide)};
        if (utf8.size() > max_content_bytes) {
          return std::nullopt;
        }
        return content_t {content_t::kind_e::text, std::move(utf8)};
      }
      if (png) {
        return content_t {content_t::kind_e::png, std::move(*png)};
      }
      if (bitmap) {
        const auto file {bmp_file_from_dib(*bitmap)};
        if (!file) {
          return std::nullopt;
        }
        if (auto encoded {encode_png(*file)}) {
          return content_t {content_t::kind_e::png, std::move(*encoded)};
        }
        BOOST_LOG(warning) << "Could not turn the image on the clipboard into a PNG file"sv;
      }
      return std::nullopt;
    }

    std::optional<std::uint32_t> write_clipboard(HWND window, const content_t &content) {
      // Prepared before the clipboard is opened, so others are not kept waiting
      std::vector<std::pair<UINT, HGLOBAL>> formats;
      auto free_unused {util::fail_guard([&formats]() {
        for (const auto &entry : formats) {
          if (entry.second) {
            GlobalFree(entry.second);
          }
        }
      })};

      if (content.kind == content_t::kind_e::text) {
        const auto wide {utf_utils::from_utf8(content.data)};
        const std::string_view bytes {reinterpret_cast<const char *>(wide.c_str()), (wide.size() + 1) * sizeof(wchar_t)};
        formats.emplace_back(CF_UNICODETEXT, global_copy(bytes));
      } else {
        UINT width {};
        UINT height {};
        const auto pixels {decode_bgra(content.data, width, height)};
        const auto dib {pixels ? dib_from_bgra(width, height, *pixels) : std::nullopt};
        if (!dib) {
          BOOST_LOG(warning) << "Could not decode the image the client copied"sv;
          return std::nullopt;
        }
        // The PNG itself too, for programs that keep transparency that way
        formats.emplace_back(CF_DIB, global_copy(*dib));
        if (png_format()) {
          formats.emplace_back(png_format(), global_copy(content.data));
        }
      }

      if (std::ranges::any_of(formats, [](const auto &format) {
            return format.second == nullptr;
          })) {
        return std::nullopt;
      }

      if (!open_clipboard(window)) {
        return std::nullopt;
      }
      {
        auto close {util::fail_guard([]() {
          CloseClipboard();
        })};
        if (!EmptyClipboard()) {
          return std::nullopt;
        }
        for (auto &[format, handle] : formats) {
          // The clipboard owns the memory once it takes it
          if (SetClipboardData(format, handle)) {
            handle = nullptr;
          }
        }
      }

      return GetClipboardSequenceNumber();
    }
  }  // namespace

  bool supported() {
    return true;
  }

  std::uint32_t sequence() {
    return GetClipboardSequenceNumber();
  }

  std::optional<content_t> read() {
    return clipboard_thread_t::instance().run<content_t>(read_clipboard);
  }

  std::optional<std::uint32_t> write(const content_t &content) {
    return clipboard_thread_t::instance().run<std::uint32_t>([content](HWND window) {
      return write_clipboard(window, content);
    });
  }
}  // namespace clipboard
