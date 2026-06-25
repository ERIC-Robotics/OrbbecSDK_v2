#pragma once
// Shared V4L2 device/format enumeration and interactive selection helpers.
// Used by orbbec_publisher and orbbec_gst_publisher.

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <linux/videodev2.h>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

#include <fmt/format.h>

namespace v4l2probe {

static inline int xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

inline std::string fourccToString(uint32_t fourcc) {
    char buf[5] = {
        static_cast<char>( fourcc        & 0xFF),
        static_cast<char>((fourcc >>  8) & 0xFF),
        static_cast<char>((fourcc >> 16) & 0xFF),
        static_cast<char>((fourcc >> 24) & 0xFF),
        '\0'
    };
    return buf;
}

inline uint32_t fourccFromString(const std::string &s) {
    if (s == "MJPEG" || s == "mjpeg") return V4L2_PIX_FMT_MJPEG;
    if (s == "YUYV"  || s == "yuyv")  return V4L2_PIX_FMT_YUYV;
    if (s == "NV12"  || s == "nv12")  return V4L2_PIX_FMT_NV12;
    return V4L2_PIX_FMT_YUYV;
}

struct FrameSize {
    uint32_t              width{0}, height{0};
    std::vector<uint32_t> fps_list;
};

struct FormatInfo {
    uint32_t               fourcc{0};
    std::string            description;
    bool                   compressed{false};
    std::vector<FrameSize> sizes;
};

struct DeviceProbeInfo {
    std::string path;
    std::string driver;
    std::string card;
    std::string bus_info;
};

inline std::vector<FormatInfo> enumerateFormats(int fd) {
    std::vector<FormatInfo> result;
    v4l2_fmtdesc fmtdesc{};
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    while (xioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
        FormatInfo fi;
        fi.fourcc      = fmtdesc.pixelformat;
        fi.description = reinterpret_cast<const char *>(fmtdesc.description);
        fi.compressed  = (fmtdesc.flags & V4L2_FMT_FLAG_COMPRESSED) != 0;

        v4l2_frmsizeenum frmsize{};
        frmsize.pixel_format = fmtdesc.pixelformat;
        frmsize.index = 0;
        while (xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) == 0) {
            if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                FrameSize fs;
                fs.width  = frmsize.discrete.width;
                fs.height = frmsize.discrete.height;

                v4l2_frmivalenum frmival{};
                frmival.pixel_format = fmtdesc.pixelformat;
                frmival.width  = fs.width;
                frmival.height = fs.height;
                frmival.index  = 0;
                while (xioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &frmival) == 0) {
                    if (frmival.type == V4L2_FRMIVAL_TYPE_DISCRETE)
                        fs.fps_list.push_back(
                            frmival.discrete.denominator / frmival.discrete.numerator);
                    ++frmival.index;
                }
                fi.sizes.push_back(std::move(fs));
            } else if (frmsize.type == V4L2_FRMSIZE_TYPE_STEPWISE) {
                FrameSize fsMin, fsMax;
                fsMin.width  = frmsize.stepwise.min_width;
                fsMin.height = frmsize.stepwise.min_height;
                fsMax.width  = frmsize.stepwise.max_width;
                fsMax.height = frmsize.stepwise.max_height;
                fi.sizes.push_back(std::move(fsMin));
                if (fsMax.width != fsMin.width || fsMax.height != fsMin.height)
                    fi.sizes.push_back(std::move(fsMax));
                break;
            }
            ++frmsize.index;
        }
        result.push_back(std::move(fi));
        ++fmtdesc.index;
    }
    return result;
}

inline std::vector<DeviceProbeInfo> scanDevices(int maxN = 20) {
    std::vector<DeviceProbeInfo> found;
    for (int n = 0; n < maxN; ++n) {
        std::string path = fmt::format("/dev/video{}", n);
        int fd = open(path.c_str(), O_RDWR | O_NONBLOCK);
        if (fd < 0) continue;
        v4l2_capability cap{};
        if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 &&
            (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
            found.push_back({
                path,
                reinterpret_cast<const char *>(cap.driver),
                reinterpret_cast<const char *>(cap.card),
                reinterpret_cast<const char *>(cap.bus_info)});
        }
        close(fd);
    }
    return found;
}

inline int promptIndex(const std::string &prompt, int maxExclusive, int defaultVal = 0) {
    std::cout << prompt << std::flush;
    std::string line;
    if (!std::getline(std::cin, line) || line.empty()) return defaultVal;
    try {
        int v = std::stoi(line);
        if (v >= 0 && v < maxExclusive) return v;
    } catch (...) {}
    return defaultVal;
}

inline std::string promptString(const std::string &prompt, const std::string &defaultVal) {
    std::cout << prompt << std::flush;
    std::string line;
    if (!std::getline(std::cin, line) || line.empty()) return defaultVal;
    return line;
}

struct Selection {
    DeviceProbeInfo device;
    std::string     name;
    FormatInfo      format;
    FrameSize       size;
    uint32_t        fps{30};
    bool            valid{false};
};

// Full interactive wizard: camera → format → resolution → fps
inline Selection interactiveSelect(const std::string &defaultName = "cam0") {
    Selection sel;

    auto found = scanDevices();
    if (found.empty()) {
        std::cerr << "[ERROR] No V4L2 capture devices found under /dev/video*\n";
        return sel;
    }

    std::cout << "\nAvailable cameras:\n";
    for (size_t i = 0; i < found.size(); ++i)
        std::cout << fmt::format("  [{}] {}  —  {} ({})\n",
            i, found[i].path, found[i].card, found[i].bus_info);

    int camIdx = promptIndex(
        fmt::format("\nSelect camera [0-{}]: ", found.size() - 1),
        static_cast<int>(found.size()), 0);
    sel.device = found[camIdx];

    sel.name = promptString(
        fmt::format("Camera name [{}]: ", defaultName),
        defaultName);

    int fd = open(sel.device.path.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        std::cerr << fmt::format("[ERROR] Cannot open {}: {}\n",
            sel.device.path, strerror(errno));
        return sel;
    }
    auto formats = enumerateFormats(fd);
    close(fd);

    if (formats.empty()) {
        std::cerr << fmt::format("[ERROR] No formats enumerated for {}\n", sel.device.path);
        return sel;
    }

    std::cout << fmt::format("\nFormats supported by {}:\n", sel.device.path);
    for (size_t i = 0; i < formats.size(); ++i) {
        std::cout << fmt::format("  [{}] {}  —  {}{}",
            i,
            fourccToString(formats[i].fourcc),
            formats[i].description,
            formats[i].compressed ? " (compressed)" : "");
        if (!formats[i].sizes.empty()) {
            std::cout << "  [";
            for (size_t j = 0; j < formats[i].sizes.size(); ++j) {
                if (j) std::cout << ", ";
                std::cout << formats[i].sizes[j].width << "x" << formats[i].sizes[j].height;
            }
            std::cout << "]";
        }
        std::cout << "\n";
    }

    int fmtIdx = promptIndex(
        fmt::format("Select format [0-{}]: ", formats.size() - 1),
        static_cast<int>(formats.size()), 0);
    sel.format = formats[fmtIdx];

    if (!sel.format.sizes.empty()) {
        std::cout << fmt::format("\nResolutions for {}:\n",
            fourccToString(sel.format.fourcc));
        for (size_t i = 0; i < sel.format.sizes.size(); ++i) {
            const auto &s = sel.format.sizes[i];
            std::string fpsStr;
            for (size_t j = 0; j < s.fps_list.size(); ++j) {
                if (j) fpsStr += ", ";
                fpsStr += std::to_string(s.fps_list[j]);
            }
            std::cout << fmt::format("  [{}] {}x{}{}",
                i, s.width, s.height,
                fpsStr.empty() ? "\n" : fmt::format("  @ {} fps\n", fpsStr));
        }

        int sizeIdx = promptIndex(
            fmt::format("Select resolution [0-{}]: ", sel.format.sizes.size() - 1),
            static_cast<int>(sel.format.sizes.size()), 0);
        sel.size = sel.format.sizes[sizeIdx];

        if (!sel.size.fps_list.empty()) {
            if (sel.size.fps_list.size() == 1) {
                sel.fps = sel.size.fps_list[0];
            } else {
                std::cout << "\nFramerates:\n";
                for (size_t i = 0; i < sel.size.fps_list.size(); ++i)
                    std::cout << fmt::format("  [{}] {} fps\n", i, sel.size.fps_list[i]);
                int fpsIdx = promptIndex(
                    fmt::format("Select framerate [0-{}]: ", sel.size.fps_list.size() - 1),
                    static_cast<int>(sel.size.fps_list.size()), 0);
                sel.fps = sel.size.fps_list[fpsIdx];
            }
        }
    }

    sel.valid = true;
    return sel;
}

} // namespace v4l2probe
