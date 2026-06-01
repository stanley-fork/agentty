#include "agentty/io/clipboard.hpp"
#include "agentty/util/env.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
#  include <unistd.h>   // getpid (Kitty clipboard temp path)
#endif

#if defined(_WIN32)
  #define AGENTTY_POPEN  ::_popen
  #define AGENTTY_PCLOSE ::_pclose
  #define AGENTTY_POPEN_MODE "rb"

  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <objidl.h>     // IStream
  #include <shlwapi.h>    // SHCreateMemStream
  #include <gdiplus.h>
#else
  #define AGENTTY_POPEN  ::popen
  #define AGENTTY_PCLOSE ::pclose
  #define AGENTTY_POPEN_MODE "r"
#endif

namespace agentty {

namespace {

// Anthropic's per-image cap is 5 MB; raw 8 MiB after base64 expansion
// is the most we'd ship. Bigger captures get truncated to 8 MiB at
// the read boundary — magic-byte sniff still works on the prefix.
constexpr std::size_t kCap = 8 * 1024 * 1024;

// CaptureResult is used by every platform — wrap() takes one, the POSIX
// helpers populate it via popen, and the Win32 path constructs it
// directly from clipboard bytes. Keep it outside the platform guard.
struct CaptureResult {
    std::string bytes;
    int         status = -1;
};

// sniff_image_type is used by wrap() on every platform — keep it
// outside the POSIX guard.
const char* sniff_image_type(std::string_view bytes) {
    auto u = [&](std::size_t i){ return static_cast<unsigned char>(bytes[i]); };
    if (bytes.size() >= 8 && u(0) == 0x89 && u(1) == 0x50 && u(2) == 0x4E
        && u(3) == 0x47 && u(4) == 0x0D && u(5) == 0x0A && u(6) == 0x1A
        && u(7) == 0x0A) return "image/png";
    if (bytes.size() >= 3 && u(0) == 0xFF && u(1) == 0xD8 && u(2) == 0xFF)
        return "image/jpeg";
    if (bytes.size() >= 6
        && bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F'
        && bytes[3] == '8' && (bytes[4] == '7' || bytes[4] == '9')
        && bytes[5] == 'a') return "image/gif";
    if (bytes.size() >= 12
        && bytes[0] == 'R' && bytes[1] == 'I' && bytes[2] == 'F'
        && bytes[3] == 'F' && bytes[8] == 'W' && bytes[9] == 'E'
        && bytes[10] == 'B' && bytes[11] == 'P') return "image/webp";
    return nullptr;
}

// Run a shell command and capture binary stdout up to `cap` bytes.
// Returns the bytes plus the wait-status. status==-1 means popen
// failed outright (rare — fork/exec issues). Available on every
// platform: Windows maps AGENTTY_POPEN → _popen at the top of the
// file, so the AGENTTY_CLIPBOARD_CMD override works there too.
CaptureResult popen_capture(const char* cmd, std::size_t cap) {
    CaptureResult r;
    FILE* fp = AGENTTY_POPEN(cmd, AGENTTY_POPEN_MODE);
    if (!fp) return r;
    r.bytes.reserve(std::min<std::size_t>(cap, 256 * 1024));
    char buf[8192];
    while (r.bytes.size() < cap) {
        std::size_t avail = cap - r.bytes.size();
        std::size_t want  = avail < sizeof(buf) ? avail : sizeof(buf);
        std::size_t n = std::fread(buf, 1, want, fp);
        if (n == 0) break;
        r.bytes.append(buf, n);
    }
    r.status = AGENTTY_PCLOSE(fp);
    return r;
}

// Image capture via a user-supplied command (AGENTTY_CLIPBOARD_CMD).
// Defined after wrap() below (it depends on it).

// The tool-detection helpers below are only used on POSIX / macOS. On
// Windows the clipboard read goes through the Win32 API directly and
// these would trip -Wunused-function.
#if !defined(_WIN32)

bool tool_in_path(const char* name) {
    std::string cmd = "command -v ";
    cmd += name;
    cmd += " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

// Pick the best image-class MIME type from a newline-separated
// listing the clipboard advertises. Preference order matches the
// quality that survives a round trip through Anthropic's image
// resizing: lossless first (PNG / WEBP-lossless), then lossy. Note
// that some Qt apps publish only `application/x-qt-image`, which
// looks image-y but doesn't actually carry decodable PNG/JPEG bytes
// — we explicitly skip it.
const char* pick_clipboard_image_type(std::string_view types) {
    static const char* prefs[] = {
        "image/png",
        "image/jpeg", "image/jpg",
        "image/webp",
        "image/gif",
        "image/bmp",
        "image/tiff",
    };
    for (const char* p : prefs) {
        if (types.find(p) != std::string_view::npos) return p;
    }
    return nullptr;
}

bool clipboard_has_qt_image_only(std::string_view types) {
    if (types.find("application/x-qt-image") == std::string_view::npos)
        return false;
    return pick_clipboard_image_type(types) == nullptr;
}

#endif // !_WIN32

std::optional<ClipboardImage> wrap(CaptureResult r) {
    if (r.status != 0 || r.bytes.empty()) return std::nullopt;
    auto* mt = sniff_image_type(r.bytes);
    if (!mt) return std::nullopt;
    ClipboardImage img;
    img.bytes      = std::move(r.bytes);
    img.media_type = mt;
    return img;
}

// Image capture via a user-supplied command (AGENTTY_CLIPBOARD_CMD).
// The override is the airgap escape hatch: the remote agentty has no
// local clipboard, so the user points this at a command that ferries
// the laptop's clipboard image back over the open SSH session (e.g.
// an `ssh` callback to the laptop's wl-paste/pbpaste). Returns:
//   - an image  → override produced decodable bytes
//   - nullopt + clip_handled=false → override unset, fall through to
//     the platform-native path
//   - nullopt + clip_handled=true  → override set but produced no
//     usable image; error_out describes why (don't fall through, the
//     user explicitly chose this path)
std::optional<ClipboardImage>
try_clipboard_cmd_override(std::string* error_out, bool& clip_handled) {
    clip_handled = false;
    const char* cmd = util::env::get_or_null<util::env::Var::ClipboardCmd>();
    if (!cmd) return std::nullopt;
    clip_handled = true;
    auto r = popen_capture(cmd, kCap);
    if (r.bytes.empty()) {
        if (error_out)
            *error_out = "AGENTTY_CLIPBOARD_CMD produced no output "
                         "(clipboard empty, or the command failed)";
        return std::nullopt;
    }
    if (auto img = wrap(std::move(r))) return img;
    if (error_out)
        *error_out = "AGENTTY_CLIPBOARD_CMD output was not a recognised "
                     "image (expected PNG/JPEG/GIF/WEBP bytes on stdout)";
    return std::nullopt;
}

// Kitty clipboard read via the `kitten clipboard` protocol. This is how
// image paste works over a plain SSH session: the `kitten` binary on
// whatever host agentty runs on speaks Kitty's clipboard escape protocol
// to the *local* Kitty terminal over the same TTY/control channel, so
// the bytes cross the SSH link without any clipboard tool on the remote.
//
// We make the kitten write to a temp FILE rather than stdout: the
// stdout/pipe form blocks waiting on the terminal handshake in ways that
// fight a TUI's raw-mode loop, whereas the file form completes and exits.
// Requires Kitty (TERM=xterm-kitty / KITTY_WINDOW_ID) and `kitten` on
// PATH. Returns nullopt (no error_out) when not under Kitty so the
// caller falls through to the native path.
std::optional<ClipboardImage> try_kitty_clipboard(std::string* error_out) {
#if !defined(_WIN32)
    // Don't gate on TERM / KITTY_WINDOW_ID: SSH strips KITTY_WINDOW_ID
    // and may rewrite TERM, so they're unreliable on a remote host.
    // Presence of the `kitten` binary plus "no native clipboard" (the
    // only context we're called in) is signal enough — if the terminal
    // on the other end of the TTY isn't actually Kitty, the kitten's
    // OSC handshake gets no reply and the `timeout` below bounds the
    // wait, after which we fall through.
    if (!tool_in_path("kitten")) return std::nullopt;

    // Unique temp path. tmpnam is racy in general but fine here: we own
    // the name, write+read+unlink it immediately, no attacker window in
    // a single-user TTY session.
    std::string tmp = "/tmp/agentty-kclip-" + std::to_string(::getpid()) + ".bin";

    // --get-clipboard --mime 'image/*' picks the first image MIME the
    // clipboard offers; kitten transcodes raster formats to what we ask.
    // We request a concrete png file so kitten emits PNG.
    //
    // `timeout` guards against a stuck terminal handshake freezing the
    // UI thread — the kitten waits on a terminal reply that may never
    // come (terminal not Kitty after all, OSC swallowed, perms denied).
    // 5s is generous for a clipboard round-trip incl. a permission tap.
    std::string have_timeout = tool_in_path("timeout") ? "timeout 5 " : "";
    std::string errlog = tmp + ".err";
    std::string cmd = have_timeout
        + "kitten clipboard --get-clipboard --mime 'image/png' "
        + tmp + " </dev/tty >/dev/tty 2>" + errlog;
    int rc = std::system(cmd.c_str());

    CaptureResult r;
    if (FILE* f = std::fopen(tmp.c_str(), "rb")) {
        char buf[8192];
        std::size_t n;
        while (r.bytes.size() < kCap && (n = std::fread(buf, 1, sizeof(buf), f)) > 0)
            r.bytes.append(buf, n);
        std::fclose(f);
        r.status = 0;
    }

    // Optional diagnostics: AGENTTY_DEBUG_FILE captures the kitten's
    // exit code, captured bytes, and its stderr so a failed paste can
    // be diagnosed without guessing.
    if (const char* dbg = util::env::get_or_null<util::env::Var::DebugFile>()) {
        std::string kerr;
        if (FILE* ef = std::fopen(errlog.c_str(), "rb")) {
            char b[1024]; std::size_t n;
            while ((n = std::fread(b, 1, sizeof(b), ef)) > 0) kerr.append(b, n);
            std::fclose(ef);
        }
        if (FILE* lf = std::fopen(dbg, "a")) {
            std::fprintf(lf,
                "[kitty-clip] cmd=%s rc=%d bytes=%zu stderr=%.500s\n",
                cmd.c_str(), rc, r.bytes.size(), kerr.c_str());
            std::fclose(lf);
        }
    }
    std::remove(errlog.c_str());
    std::remove(tmp.c_str());

    if (r.bytes.empty()) {
        if (error_out)
            *error_out = "Kitty clipboard had no image "
                         "(copy an image, then Ctrl+V; if a permission "
                         "prompt appeared, accept it and retry)";
        return std::nullopt;
    }
    if (auto img = wrap(std::move(r))) return img;
    if (error_out)
        *error_out = "Kitty clipboard returned non-image data";
    return std::nullopt;
#else
    (void)error_out;
    return std::nullopt;
#endif
}

} // namespace

std::optional<ClipboardImage> read_clipboard_image(std::string* error_out) {
    auto fail = [&](const char* msg) -> std::optional<ClipboardImage> {
        if (error_out) *error_out = msg;
        return std::nullopt;
    };
    auto fail_owned = [&](std::string msg) -> std::optional<ClipboardImage> {
        if (error_out) *error_out = std::move(msg);
        return std::nullopt;
    };

    // AGENTTY_CLIPBOARD_CMD override runs FIRST, before any platform
    // probe. This is the airgap path: the remote host has no clipboard
    // of its own, so the override ferries the laptop's clipboard image
    // back over the open SSH session. When set, it is authoritative —
    // we don't fall through to wl-paste/xclip/etc. (those would only
    // find the empty remote clipboard and clobber the precise error).
    {
        bool handled = false;
        if (auto img = try_clipboard_cmd_override(error_out, handled))
            return img;
        if (handled) return std::nullopt;  // error_out already set
    }

#if defined(__linux__)
    // Session-type detection. KDE / GNOME / sway / etc. on Wayland
    // all set XDG_SESSION_TYPE; the WAYLAND_DISPLAY fallback catches
    // edge cases where the env var got unset by a shell rc.
    bool wayland = false;
    if (const char* st = std::getenv("XDG_SESSION_TYPE"))
        wayland = std::string_view{st} == "wayland";
    if (const char* w = std::getenv("WAYLAND_DISPLAY"); w && *w) wayland = true;

    bool has_wl_paste = tool_in_path("wl-paste");
    bool has_xclip    = tool_in_path("xclip");

    if (!has_wl_paste && !has_xclip) {
        // No display server reachable usually means a headless / SSH /
        // airgap host. The image is on the user's laptop, not here. If
        // we're running under Kitty, the kitten can fetch it over the
        // terminal's own clipboard protocol — works in a plain SSH
        // session, no clipboard tool needed on this host.
        std::string kitty_err;
        if (auto img = try_kitty_clipboard(&kitty_err)) return img;

        const char* disp = std::getenv("DISPLAY");
        const char* wl   = std::getenv("WAYLAND_DISPLAY");
        const bool headless = (!disp || !*disp) && (!wl || !*wl);
        if (headless) {
            if (!kitty_err.empty()) return fail_owned(std::move(kitty_err));
            return fail(
                "no clipboard on this host (headless / SSH). The image is "
                "on your laptop, not the remote. Use a Kitty terminal (its "
                "`kitten` fetches the clipboard over SSH), set "
                "AGENTTY_CLIPBOARD_CMD, or attach the image by path");
        }
        return fail(wayland
            ? "no clipboard tool — install wl-clipboard "
              "(`pacman -S wl-clipboard` / `apt install wl-clipboard`)"
            : "no clipboard tool — install xclip "
              "(`pacman -S xclip` / `apt install xclip`)");
    }

    // ---- Wayland path: prefer wl-paste --------------------------------
    if (wayland && has_wl_paste) {
        // Discover what the clipboard is actually offering. wl-paste
        // exits non-zero on an empty clipboard; capture even on
        // failure so we can give a precise diagnostic.
        auto types_r = popen_capture("wl-paste --list-types 2>/dev/null", 64 * 1024);
        if (types_r.bytes.empty()) {
            // wl-paste returned nothing — empty clipboard, OR
            // wl-paste needs a connection it can't get.
            // Try xclip fallback if installed.
            if (!has_xclip)
                return fail("clipboard is empty");
            // fall through to xclip path below
        } else if (auto* mime = pick_clipboard_image_type(types_r.bytes)) {
            std::string cmd = "wl-paste --type ";
            cmd += mime;
            cmd += " 2>/dev/null";
            if (auto img = wrap(popen_capture(cmd.c_str(), kCap)))
                return img;
            // Listed but failed to capture — usually means the
            // source app died between list and read (KDE's Klipper
            // can race here on Wayland).
            return fail_owned(std::string{
                "clipboard advertised "} + mime
                + " but the bytes were unavailable (source app may have closed)");
        } else if (clipboard_has_qt_image_only(types_r.bytes)) {
            return fail("clipboard image is in Qt-internal format only "
                        "(application/x-qt-image) — copy from a non-Qt app, "
                        "or take the screenshot via Spectacle's \"Save to "
                        "clipboard\" with the PNG default");
        }
        // No image-class MIME on the wayland clipboard. Fall through
        // to xclip in case Klipper's X11 bridge has it.
    }

    // ---- X11 path (also runs as fallback on Wayland) ------------------
    if (has_xclip) {
        auto targets = popen_capture(
            "xclip -selection clipboard -t TARGETS -o 2>/dev/null",
            64 * 1024);
        if (auto* mime = pick_clipboard_image_type(targets.bytes)) {
            std::string cmd = "xclip -selection clipboard -t ";
            cmd += mime;
            cmd += " -o 2>/dev/null";
            if (auto img = wrap(popen_capture(cmd.c_str(), kCap)))
                return img;
        }
        if (clipboard_has_qt_image_only(targets.bytes)) {
            return fail("clipboard image is in Qt-internal format only "
                        "(application/x-qt-image) — install wl-clipboard for "
                        "Wayland-native access");
        }
    }

    // Both tools tried; nothing image-y came back.
    if (wayland && !has_wl_paste) {
        return fail("clipboard has no image — Wayland session needs "
                    "wl-clipboard for native access (xclip works only "
                    "via Klipper's X11 bridge, which doesn't always "
                    "carry images)");
    }
    return fail("clipboard has no image");

#elif defined(__APPLE__)
    if (tool_in_path("pngpaste")) {
        if (auto img = wrap(popen_capture("pngpaste - 2>/dev/null", kCap)))
            return img;
        return fail("clipboard has no image");
    }
    // osascript fallback — slower (~150-300 ms) but always available.
    auto r = popen_capture(
        "set -e; "
        "f=$(mktemp -t agentty-clip).png; "
        "trap 'rm -f \"$f\"' EXIT; "
        "osascript -e 'set png to (the clipboard as «class PNGf»)' "
        "          -e 'set fh to open for access POSIX file \"'\"$f\"'\" "
        "                 with write permission' "
        "          -e 'write png to fh' "
        "          -e 'close access fh' >/dev/null 2>&1 && cat \"$f\"",
        kCap);
    if (auto img = wrap(std::move(r))) return img;
    return fail("clipboard has no image (install pngpaste for a faster path: "
                "`brew install pngpaste`)");

#elif defined(_WIN32)
    // Direct Win32 Clipboard API. The previous PowerShell shell-out
    // had two failure modes that made Ctrl+V silently no-op:
    //
    //   1. Quoting: _popen launches `cmd /C <our string>`. The full
    //      script contained `(`, `)`, `;`, `$`, `[`, `]` — characters
    //      cmd treats as either special or shell-meta depending on the
    //      parent process's environment. Under MSYS2 / Git Bash, the
    //      command was visibly mangled before reaching powershell.
    //
    //   2. PowerShell startup tax: even when quoting worked, the user
    //      paid 200–500 ms of PS spin-up per paste. On a healthy
    //      clipboard the diagnostic was `wrap()` returning nullopt
    //      because of a non-zero status, which surfaced as the generic
    //      "no image on clipboard" toast — hiding the real cause.
    //
    // Going through user32/gdiplus eliminates the shell entirely. ~ms-
    // scale paste, precise errors, no external dependency.

    if (!::OpenClipboard(nullptr))
        return fail("could not open Windows clipboard (another process may hold it)");
    struct ClipGuard {
        ~ClipGuard() { ::CloseClipboard(); }
    } clip_guard;

    // (a) Fast path: clipboard already carries native PNG bytes. Browsers,
    //     Discord, Slack, ShareX, Greenshot, etc. register the "PNG"
    //     format alongside CF_DIB so we can copy without re-encoding.
    if (UINT png_fmt = ::RegisterClipboardFormatW(L"PNG");
        png_fmt && ::IsClipboardFormatAvailable(png_fmt))
    {
        if (HANDLE h = ::GetClipboardData(png_fmt); h) {
            auto* data = static_cast<const char*>(::GlobalLock(h));
            const SIZE_T size = data ? ::GlobalSize(h) : 0;
            if (data && size > 0) {
                std::string bytes(data,
                                  std::min<std::size_t>(size, kCap));
                ::GlobalUnlock(h);
                if (auto img = wrap(CaptureResult{std::move(bytes), 0}))
                    return img;
            } else if (data) {
                ::GlobalUnlock(h);
            }
        }
    }

    // GDI+ scope: shared across (b) DIB path and (c) CF_BITMAP fallback.
    // Per-call startup; a process-wide token would shave ~1 ms but adds
    // lifecycle the rest of the app doesn't need.
    Gdiplus::GdiplusStartupInput gdi_in;
    ULONG_PTR gdi_token = 0;
    if (Gdiplus::GdiplusStartup(&gdi_token, &gdi_in, nullptr) != Gdiplus::Ok)
        return fail("GDI+ startup failed");
    struct GdiGuard {
        ULONG_PTR tok;
        ~GdiGuard() { Gdiplus::GdiplusShutdown(tok); }
    } gdi_guard{gdi_token};

    // Lazy-found PNG encoder CLSID. There's no GetEncoderByMime helper —
    // walk the codec list once and remember the result for both paths.
    auto find_png_encoder = [&](CLSID& out) -> bool {
        UINT num = 0, sz = 0;
        Gdiplus::GetImageEncodersSize(&num, &sz);
        if (num == 0 || sz == 0) return false;
        std::vector<BYTE> buf(sz);
        auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
        Gdiplus::GetImageEncoders(num, sz, codecs);
        for (UINT i = 0; i < num; ++i) {
            if (std::wcscmp(codecs[i].MimeType, L"image/png") == 0) {
                out = codecs[i].Clsid;
                return true;
            }
        }
        return false;
    };

    // Encode a GDI+ Bitmap to PNG bytes via an in-memory IStream.
    auto encode_to_png = [&](Gdiplus::Bitmap& bitmap,
                             std::string& out_bytes,
                             std::string& out_err) -> bool
    {
        CLSID png_clsid{};
        if (!find_png_encoder(png_clsid)) {
            out_err = "GDI+ PNG encoder not registered on this system";
            return false;
        }
        IStream* out_stream = ::SHCreateMemStream(nullptr, 0);
        if (!out_stream) { out_err = "SHCreateMemStream(out) failed"; return false; }
        struct StreamRelease { IStream* s; ~StreamRelease(){ if(s) s->Release(); } } sg{out_stream};

        if (bitmap.Save(out_stream, &png_clsid, nullptr) != Gdiplus::Ok) {
            out_err = "GDI+ PNG encode failed";
            return false;
        }
        STATSTG stat{};
        if (out_stream->Stat(&stat, STATFLAG_NONAME) != S_OK) {
            out_err = "PNG stream Stat failed";
            return false;
        }
        const auto png_size = static_cast<std::size_t>(stat.cbSize.QuadPart);
        if (png_size == 0 || png_size > kCap) {
            out_err = "PNG output size out of range";
            return false;
        }
        LARGE_INTEGER zero{};
        out_stream->Seek(zero, STREAM_SEEK_SET, nullptr);
        out_bytes.assign(png_size, '\0');
        ULONG bytes_read = 0;
        if (out_stream->Read(out_bytes.data(),
                             static_cast<ULONG>(png_size),
                             &bytes_read) != S_OK) {
            out_err = "PNG stream Read failed";
            return false;
        }
        out_bytes.resize(bytes_read);
        return true;
    };

    // (b) DIB fallback: the standard "Win+Shift+S / Snipping Tool /
    //     PrintScreen" path. Re-attach a BITMAPFILEHEADER so the bytes
    //     are a complete BMP file, then re-encode through GDI+ to PNG
    //     (Anthropic's image API doesn't accept BMP).

    if (const UINT dib_fmt =
              ::IsClipboardFormatAvailable(CF_DIBV5) ? CF_DIBV5
            : ::IsClipboardFormatAvailable(CF_DIB)   ? CF_DIB
            : 0u;
        dib_fmt != 0)
    {
        HANDLE dh = ::GetClipboardData(dib_fmt);
        if (dh) {
            auto* dib = static_cast<const BYTE*>(::GlobalLock(dh));
            const SIZE_T dib_size = dib ? ::GlobalSize(dh) : 0;
            struct UnlockGuard {
                HANDLE h;
                ~UnlockGuard() { if (h) ::GlobalUnlock(h); }
            } unlock_guard{dh};
            if (dib && dib_size >= sizeof(BITMAPINFOHEADER)) {
                // Compute where the pixel array starts inside the DIB
                // block. The hairy bit: bitfield masks only appear as a
                // separate trailing block when the header is the V3
                // BITMAPINFOHEADER (size 40). V4 (108) and V5 (124)
                // store the masks INLINE, so adding 12 bytes shifts
                // every pixel of a modern Win+Shift+S screenshot —
                // Snipping Tool writes CF_DIBV5 with
                // biCompression=BI_BITFIELDS, which is exactly the case
                // the previous code mis-handled.
                const auto* hdr = reinterpret_cast<const BITMAPINFOHEADER*>(dib);
                DWORD masks_bytes = 0;
                if (hdr->biSize == sizeof(BITMAPINFOHEADER)) {
                    if (hdr->biCompression == BI_BITFIELDS)
                        masks_bytes = 3 * sizeof(DWORD);
                    else if (hdr->biCompression == 6 /* BI_ALPHABITFIELDS */)
                        masks_bytes = 4 * sizeof(DWORD);
                }
                DWORD palette_bytes = 0;
                if (hdr->biBitCount <= 8) {
                    const DWORD n = hdr->biClrUsed ? hdr->biClrUsed
                                                   : (1u << hdr->biBitCount);
                    palette_bytes = n * sizeof(RGBQUAD);
                }
                const DWORD pixels_offset =
                    static_cast<DWORD>(sizeof(BITMAPFILEHEADER))
                  + hdr->biSize + masks_bytes + palette_bytes;

                std::string bmp;
                bmp.resize(sizeof(BITMAPFILEHEADER) + dib_size);
                BITMAPFILEHEADER bfh{};
                bfh.bfType    = 0x4D42; // 'BM'
                bfh.bfSize    = static_cast<DWORD>(bmp.size());
                bfh.bfOffBits = pixels_offset;
                std::memcpy(bmp.data(), &bfh, sizeof(bfh));
                std::memcpy(bmp.data() + sizeof(bfh), dib, dib_size);

                IStream* bmp_stream = ::SHCreateMemStream(
                    reinterpret_cast<const BYTE*>(bmp.data()),
                    static_cast<UINT>(bmp.size()));
                if (bmp_stream) {
                    struct StreamRelease { IStream* s; ~StreamRelease(){ if(s) s->Release(); } } bg{bmp_stream};
                    std::unique_ptr<Gdiplus::Bitmap> bitmap{
                        Gdiplus::Bitmap::FromStream(bmp_stream)};
                    if (bitmap && bitmap->GetLastStatus() == Gdiplus::Ok) {
                        std::string png_bytes, enc_err;
                        if (encode_to_png(*bitmap, png_bytes, enc_err)) {
                            if (auto img = wrap(CaptureResult{std::move(png_bytes), 0}))
                                return img;
                        }
                    }
                }
            }
        }
        // Fall through to CF_BITMAP — some sources publish a malformed
        // DIB but a valid HBITMAP for the same pixels.
    }

    // (c) CF_BITMAP fallback: a few apps only register an HBITMAP, even
    //     though Windows is supposed to auto-synthesize CF_DIB from it.
    //     FromHBITMAP does the offset math for us.
    if (::IsClipboardFormatAvailable(CF_BITMAP)) {
        if (auto hbm = static_cast<HBITMAP>(::GetClipboardData(CF_BITMAP)); hbm) {
            std::unique_ptr<Gdiplus::Bitmap> bitmap{
                Gdiplus::Bitmap::FromHBITMAP(hbm, nullptr)};
            if (bitmap && bitmap->GetLastStatus() == Gdiplus::Ok) {
                std::string png_bytes, enc_err;
                if (encode_to_png(*bitmap, png_bytes, enc_err)) {
                    if (auto img = wrap(CaptureResult{std::move(png_bytes), 0}))
                        return img;
                }
                return fail_owned("CF_BITMAP path failed: " +
                                  (enc_err.empty() ? std::string{"PNG sniff failed"} : enc_err));
            }
        }
    }

    return fail("clipboard has no image (no PNG/DIBV5/DIB/CF_BITMAP format present)");
#else
    return fail("clipboard image read not implemented on this platform");
#endif
}

// ===========================================================================
// read_clipboard_text — plain text variant for the smart-paste path
// ===========================================================================

std::optional<std::string> read_clipboard_text(std::string* error_out) {
    auto fail = [&](const char* msg) -> std::optional<std::string> {
        if (error_out) *error_out = msg;
        return std::nullopt;
    };

#if defined(__linux__)
    bool wayland = false;
    if (const char* st = std::getenv("XDG_SESSION_TYPE"))
        wayland = std::string_view{st} == "wayland";
    if (const char* w = std::getenv("WAYLAND_DISPLAY"); w && *w) wayland = true;

    if (wayland && tool_in_path("wl-paste")) {
        auto r = popen_capture("wl-paste --no-newline 2>/dev/null", kCap);
        if (r.status == 0 && !r.bytes.empty()) return std::move(r.bytes);
    }
    if (tool_in_path("xclip")) {
        auto r = popen_capture(
            "xclip -selection clipboard -o 2>/dev/null", kCap);
        if (r.status == 0 && !r.bytes.empty()) return std::move(r.bytes);
    }
    return fail("clipboard has no text");

#elif defined(__APPLE__)
    auto r = popen_capture("pbpaste 2>/dev/null", kCap);
    if (r.status == 0 && !r.bytes.empty()) return std::move(r.bytes);
    return fail("clipboard has no text");

#elif defined(_WIN32)
    if (!::OpenClipboard(nullptr))
        return fail("could not open Windows clipboard");
    struct ClipGuard {
        ~ClipGuard() { ::CloseClipboard(); }
    } clip_guard;

    // CF_UNICODETEXT first — modern apps write Unicode. CF_TEXT is the
    // legacy fallback (system-codepage), but Windows auto-synthesises
    // it from CF_UNICODETEXT and vice-versa, so checking UNICODETEXT
    // alone covers nearly everything.
    if (::IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        if (HANDLE h = ::GetClipboardData(CF_UNICODETEXT); h) {
            auto* wide = static_cast<const wchar_t*>(::GlobalLock(h));
            struct UnlockGuard {
                HANDLE h;
                ~UnlockGuard() { if (h) ::GlobalUnlock(h); }
            } unlock_guard{h};
            if (!wide) return fail("GlobalLock(CF_UNICODETEXT) failed");

            // wcslen — clipboard text is NUL-terminated by the
            // Windows clipboard contract.
            const int wide_len = static_cast<int>(std::wcslen(wide));
            if (wide_len == 0) return fail("clipboard text is empty");

            const int utf8_len = ::WideCharToMultiByte(
                CP_UTF8, 0, wide, wide_len, nullptr, 0, nullptr, nullptr);
            if (utf8_len <= 0) return fail("UTF-8 conversion failed");
            std::string out(static_cast<std::size_t>(utf8_len), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, wide, wide_len,
                                  out.data(), utf8_len, nullptr, nullptr);
            return out;
        }
    }
    return fail("clipboard has no text");

#else
    return fail("clipboard text read not implemented on this platform");
#endif
}

} // namespace agentty
