/**
 * @file src/platform/windows/virtual_display.cpp
 * @brief The SudoVDA driver behind the virtual display backend interface.
 */
// standard includes
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

// platform includes
#include <windows.h>
// Must follow windows.h
#include <setupapi.h>
// Must follow windows.h
#include <dxgi.h>

// lib includes
#include <sudovda-ioctl.h>

// local includes
#include "src/logging.h"
#include "src/virtual_display.h"
#include "utf_utils.h"

using namespace std::literals;

namespace virtual_display {

  namespace {

    /**
     * @brief What one device call returned.
     *
     * The byte count is kept because a driver that answers with a short buffer
     * has not told us what we asked for, and reading the untouched remainder
     * of the output struct would be reading uninitialised memory.
     */
    struct io_result_t {
      bool ok {};
      DWORD error {};
      DWORD bytes_returned {};
    };

    /**
     * @brief How many hertz two refresh rates may differ by and still match.
     *
     * Windows reports whole hertz, so 59.94Hz and 60Hz are both 60, and a
     * driver offering one of them satisfies a request for the other.
     */
    constexpr int refresh_rate_tolerance_hz = 1;

    /**
     * @brief Convert the driver's thousandths of a hertz to whole hertz.
     *
     * @param millihz Refresh rate in thousandths of a hertz.
     * @return The nearest whole hertz.
     */
    int to_whole_hz(int millihz) {
      return (millihz + 500) / 1000;
    }

    /**
     * @brief The SudoVDA driver.
     */
    class sudovda_backend_t: public backend_t {
    public:
      ~sudovda_backend_t() override {
        close();
      }

      bool open() override {
        std::lock_guard lock {m_mutex};
        if (m_handle != INVALID_HANDLE_VALUE) {
          return true;
        }

        m_handle = open_interface();
        return m_handle != INVALID_HANDLE_VALUE;
      }

      void close() override {
        std::lock_guard lock {m_mutex};
        if (m_handle == INVALID_HANDLE_VALUE) {
          return;
        }

        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
        m_targets.clear();
      }

      bool protocol_supported() override {
        SUDOVDA::VIRTUAL_DISPLAY_GET_PROTOCOL_VERSION_OUT out {};
        const auto result = call(IOCTL_GET_PROTOCOL_VERSION, nullptr, 0, &out, sizeof(out));
        if (!result.ok) {
          BOOST_LOG(error) << "Virtual display driver would not report its protocol version (error "sv
                           << result.error << ')';
          return false;
        }
        if (result.bytes_returned != sizeof(out)) {
          BOOST_LOG(error) << "Virtual display driver answered the protocol query with "sv
                           << result.bytes_returned << " bytes instead of "sv << sizeof(out);
          return false;
        }

        // A different major version is a different interface. Within one, the
        // driver only adds, so a driver at or past the minor version this
        // build was written against understands everything it is sent.
        const auto &theirs = out.Version;
        const auto &ours = SUDOVDA::VDAProtocolVersion;
        if (theirs.Major != ours.Major || theirs.Minor < ours.Minor) {
          BOOST_LOG(error) << "Virtual display driver speaks protocol "sv
                           << (int) theirs.Major << '.' << (int) theirs.Minor
                           << ", which this build cannot use. Expected "sv
                           << (int) ours.Major << '.' << (int) ours.Minor << " or a later minor version."sv;
          return false;
        }

        return true;
      }

      std::optional<std::chrono::seconds> watchdog_timeout() override {
        SUDOVDA::VIRTUAL_DISPLAY_GET_WATCHDOG_OUT out {};
        const auto result = call(IOCTL_GET_WATCHDOG, nullptr, 0, &out, sizeof(out));
        if (!result.ok || result.bytes_returned != sizeof(out)) {
          BOOST_LOG(error) << "Virtual display driver would not report its watchdog timeout"sv;
          return std::nullopt;
        }

        return std::chrono::seconds {out.Timeout};
      }

      bool ping() override {
        return call(IOCTL_DRIVER_PING, nullptr, 0, nullptr, 0).ok;
      }

      void set_render_adapter(const std::string &adapter_name) override {
        IDXGIFactory1 *factory = nullptr;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
          return;
        }

        const auto wanted = utf_utils::from_utf8(adapter_name);
        IDXGIAdapter *adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
          DXGI_ADAPTER_DESC desc {};
          const bool match = SUCCEEDED(adapter->GetDesc(&desc)) && wanted == desc.Description;
          adapter->Release();
          adapter = nullptr;

          if (!match) {
            continue;
          }

          SUDOVDA::VIRTUAL_DISPLAY_SET_RENDER_ADAPTER_PARAMS params {desc.AdapterLuid};
          if (!call(IOCTL_SET_RENDER_ADAPTER, &params, sizeof(params), nullptr, 0).ok) {
            BOOST_LOG(warning) << "Virtual display driver would not render on "sv << adapter_name;
          }
          break;
        }

        factory->Release();
      }

      creation_e add(const uuid_util::uuid_t &id, const std::string &client_name, const std::string &client_uid, const mode_t &mode) override {
        SUDOVDA::VIRTUAL_DISPLAY_ADD_PARAMS params {
          static_cast<UINT>(mode.width),
          static_cast<UINT>(mode.height),
          static_cast<UINT>(mode.refresh_rate_millihz),
          to_guid(id),
          {},
          {}
        };
        set_field(params.DeviceName, client_name);
        set_field(params.SerialNumber, client_uid);

        SUDOVDA::VIRTUAL_DISPLAY_ADD_OUT out {};
        const auto result = call(IOCTL_ADD_VIRTUAL_DISPLAY, &params, sizeof(params), &out, sizeof(out));
        if (!result.ok) {
          BOOST_LOG(error) << "Virtual display driver refused to add a display (error "sv << result.error << ')';
          return creation_e::not_created;
        }
        if (result.bytes_returned != sizeof(out)) {
          // The driver took the request, so a display may well exist, but the
          // answer is too short to hold the adapter and target needed to find
          // it. Reported as created so the caller removes it rather than
          // leaving a display nothing can reach.
          BOOST_LOG(error) << "Virtual display driver answered the add request with "sv
                           << result.bytes_returned << " bytes instead of "sv << sizeof(out);
          return creation_e::created_unverifiable;
        }

        std::lock_guard lock {m_mutex};
        m_targets[id.string()] = out;
        return creation_e::created;
      }

      bool remove(const uuid_util::uuid_t &id) override {
        SUDOVDA::VIRTUAL_DISPLAY_REMOVE_PARAMS params {to_guid(id)};
        const auto result = call(IOCTL_REMOVE_VIRTUAL_DISPLAY, &params, sizeof(params), nullptr, 0);

        {
          std::lock_guard lock {m_mutex};
          m_targets.erase(id.string());
        }

        return result.ok;
      }

      resolution_t resolve(const uuid_util::uuid_t &id, const mode_t &mode) override {
        SUDOVDA::VIRTUAL_DISPLAY_ADD_OUT target {};
        {
          std::lock_guard lock {m_mutex};
          const auto found = m_targets.find(id.string());
          if (found == m_targets.end()) {
            // Only displays this backend created are ever looked up, so an
            // unknown id is a caller mistake rather than a slow display.
            return {};
          }
          target = found->second;
        }

        const auto identity = find_path(target.AdapterLuid, target.TargetId);
        if (!identity) {
          return {};
        }

        if (!mode_offered(identity->gdi_name_w, mode)) {
          return {resolution_t::readiness_e::mode_missing, {}};
        }

        return {resolution_t::readiness_e::ready, {{}, identity->gdi_name}};
      }

    private:
      /**
       * @brief What Windows knows about a display path.
       */
      struct identity_t {
        std::string gdi_name;  ///< `\\.\DISPLAY1` style name.
        std::wstring gdi_name_w;  ///< The same, for the mode enumeration API.
        std::string device_path;  ///< Stable monitor device path, which is the id Sunshine addresses displays by.
      };

      /**
       * @brief Issue one buffered device call.
       *
       * @param code IOCTL code from the vendored header.
       * @param in Input buffer, may be null.
       * @param in_size Input buffer size.
       * @param out Output buffer, may be null.
       * @param out_size Output buffer size.
       * @return Whether the driver answered, its error, and how much it wrote.
       */
      io_result_t call(DWORD code, void *in, DWORD in_size, void *out, DWORD out_size) {
        std::lock_guard lock {m_mutex};
        if (m_handle == INVALID_HANDLE_VALUE) {
          return {false, static_cast<DWORD>(ERROR_INVALID_HANDLE), 0};
        }

        io_result_t result;
        const BOOL ok = DeviceIoControl(m_handle, code, in, in_size, out, out_size, &result.bytes_returned, nullptr);
        result.ok = ok != FALSE;
        result.error = result.ok ? ERROR_SUCCESS : GetLastError();
        return result;
      }

      /**
       * @brief Open the driver's device interface.
       *
       * @return An open handle, or INVALID_HANDLE_VALUE.
       */
      static HANDLE open_interface() {
        HDEVINFO device_info = SetupDiGetClassDevsA(
          &SUDOVDA::SUVDA_INTERFACE_GUID,
          nullptr,
          nullptr,
          DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
        );
        if (device_info == INVALID_HANDLE_VALUE) {
          return INVALID_HANDLE_VALUE;
        }

        HANDLE handle = INVALID_HANDLE_VALUE;
        SP_DEVICE_INTERFACE_DATA interface_data {};
        interface_data.cbSize = sizeof(interface_data);

        for (DWORD i = 0;
             SetupDiEnumDeviceInterfaces(device_info, nullptr, &SUDOVDA::SUVDA_INTERFACE_GUID, i, &interface_data);
             ++i) {
          DWORD detail_size = 0;
          SetupDiGetDeviceInterfaceDetailA(device_info, &interface_data, nullptr, 0, &detail_size, nullptr);
          if (detail_size == 0) {
            continue;
          }

          std::vector<char> buffer(detail_size);
          auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_A *>(buffer.data());
          detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

          if (!SetupDiGetDeviceInterfaceDetailA(device_info, &interface_data, detail, detail_size, &detail_size, nullptr)) {
            continue;
          }

          handle = CreateFileA(
            detail->DevicePath,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
          );
          if (handle != INVALID_HANDLE_VALUE) {
            break;
          }
        }

        SetupDiDestroyDeviceInfoList(device_info);
        return handle;
      }

      /**
       * @brief Find the active display path for an adapter and target.
       *
       * The driver answers an add with an adapter and a target, which is what
       * identifies the display to Windows. The names the rest of Sunshine
       * works in have to be looked up, and only an active path has them.
       *
       * @param luid Adapter the driver reported.
       * @param target_id Target the driver reported.
       * @return The names, or nothing while the path is not active or incomplete.
       */
      static std::optional<identity_t> find_path(const LUID &luid, UINT target_id) {
        UINT32 path_count = 0;
        UINT32 mode_count = 0;
        if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) {
          return std::nullopt;
        }

        std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
        if (QueryDisplayConfig(QDC_ALL_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr) != ERROR_SUCCESS) {
          return std::nullopt;
        }
        paths.resize(path_count);

        for (const auto &path : paths) {
          if (path.targetInfo.adapterId.LowPart != luid.LowPart ||
              path.targetInfo.adapterId.HighPart != luid.HighPart ||
              path.targetInfo.id != target_id) {
            continue;
          }

          if (!(path.flags & DISPLAYCONFIG_PATH_ACTIVE)) {
            return std::nullopt;
          }

          DISPLAYCONFIG_SOURCE_DEVICE_NAME source {};
          source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
          source.header.size = sizeof(source);
          source.header.adapterId = path.sourceInfo.adapterId;
          source.header.id = path.sourceInfo.id;
          if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) {
            return std::nullopt;
          }

          DISPLAYCONFIG_TARGET_DEVICE_NAME name {};
          name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
          name.header.size = sizeof(name);
          name.header.adapterId = path.targetInfo.adapterId;
          name.header.id = path.targetInfo.id;
          if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS) {
            return std::nullopt;
          }

          identity_t identity;
          identity.gdi_name_w = source.viewGdiDeviceName;
          identity.gdi_name = utf_utils::to_utf8(identity.gdi_name_w);
          // Not the id Sunshine addresses the display by, but a display
          // without one is not fully up yet, so it is worth checking.
          identity.device_path = utf_utils::to_utf8(name.monitorDevicePath);

          if (identity.gdi_name.empty() || identity.device_path.empty()) {
            return std::nullopt;
          }

          return identity;
        }

        return std::nullopt;
      }

      /**
       * @brief Whether a display offers a mode.
       *
       * A display that came up at some other size would stream at that size,
       * so the mode the client asked for has to be among the ones Windows
       * enumerates for it before the display counts as ready.
       *
       * @param gdi_name Display to enumerate.
       * @param mode Mode to look for.
       * @return True if the display offers it.
       */
      static bool mode_offered(const std::wstring &gdi_name, const mode_t &mode) {
        const auto wanted_hz = to_whole_hz(mode.refresh_rate_millihz);

        DEVMODEW dev_mode {};
        dev_mode.dmSize = sizeof(dev_mode);

        for (DWORD i = 0; EnumDisplaySettingsW(gdi_name.c_str(), i, &dev_mode); ++i) {
          if (static_cast<int>(dev_mode.dmPelsWidth) != mode.width ||
              static_cast<int>(dev_mode.dmPelsHeight) != mode.height) {
            continue;
          }

          if (std::abs(static_cast<int>(dev_mode.dmDisplayFrequency) - wanted_hz) <= refresh_rate_tolerance_hz) {
            return true;
          }
        }

        return false;
      }

      /**
       * @brief Reinterpret a UUID as the GUID the driver takes.
       *
       * @param id UUID to convert.
       * @return The same 16 bytes as a GUID.
       */
      static GUID to_guid(const uuid_util::uuid_t &id) {
        GUID guid {};
        static_assert(sizeof(guid) == sizeof(id.b8), "A GUID and a UUID must be the same 16 bytes");
        std::memcpy(&guid, id.b8, sizeof(guid));
        return guid;
      }

      /**
       * @brief Copy a string into a fixed driver field, truncating and terminating.
       *
       * @param field Destination.
       * @param value Source.
       */
      template<std::size_t N>
      static void set_field(CHAR (&field)[N], const std::string &value) {
        const auto length = std::min(value.size(), N - 1);
        std::memcpy(field, value.data(), length);
        field[length] = '\0';
      }

      std::mutex m_mutex;  ///< Guards the handle and the target map.
      HANDLE m_handle {INVALID_HANDLE_VALUE};
      std::map<std::string, SUDOVDA::VIRTUAL_DISPLAY_ADD_OUT> m_targets;  ///< Displays this backend created, by UUID.
    };

  }  // namespace

  std::unique_ptr<backend_t> make_backend() {
    return std::make_unique<sudovda_backend_t>();
  }

}  // namespace virtual_display
