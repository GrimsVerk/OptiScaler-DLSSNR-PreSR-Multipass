#pragma once

// Test harness for the HUD-less finished picture ([DlssNr] HudlessTestControl=true only).
//  - Live settings: nr-test-control.txt next to the DLL is polled; key=value lines change the HUD-less
//    smoothing/motion settings without a restart, and nr-test-control.ack acknowledges each "seq".
//  - Crop recorder: "record=<name>" with "frames=<n>" and "crops=x,y,w,h;..." records, for every real frame,
//    the HUD-less picture before NR, NR's output, the adjusted output and the game's motion vectors under each
//    crop, through a ring of readback buffers written to disk on a separate thread (no CPU waits on the GPU).
// Nothing here runs unless the INI switch is on.

#include <Config.h>
#include <Util.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace DlssNr::HudlessTest
{
struct Rect
{
    int x = 0, y = 0, w = 0, h = 0;
};

struct Source
{
    ID3D12Resource* resource = nullptr;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};

struct FrameInfo
{
    uint64_t gameFrame = 0;
    double milliseconds = 0;
    float mvScaleX = 1, mvScaleY = 1;
    uint32_t outputWidth = 0, outputHeight = 0;
    uint32_t guideX = 0, guideY = 0, guideWidth = 0, guideHeight = 0;
    uint32_t reset = 0, historyValid = 0, temporal = 0, adaptive = 0, smoothing = 0;
    float stillStrength = 1, motionStrength = 1, motionStart = 0, motionFull = 0, frames = 1, tolerance = 0;
    float reject = 0;
    uint32_t bandSplit = 0;
};

// One recorded frame on disk: this header, then per crop: before NR, NR, adjusted output (crop size, colour bytes),
// then the motion vectors under the crop (guide crop size, motion bytes). Rows tightly packed.
#pragma pack(push, 1)
struct FrameHeader
{
    uint32_t magic = 0x46524c48; // "HLRF"
    uint32_t index = 0;
    uint64_t gameFrame = 0;
    double milliseconds = 0;
    float mvScaleX = 1, mvScaleY = 1;
    uint32_t outputWidth = 0, outputHeight = 0;
    uint32_t guideX = 0, guideY = 0, guideWidth = 0, guideHeight = 0;
    uint32_t reset = 0, historyValid = 0, temporal = 0, adaptive = 0, smoothing = 0;
    float stillStrength = 1, motionStrength = 1, motionStart = 0, motionFull = 0, frames = 1, tolerance = 0;
    float reject = 0;
    uint32_t bandSplit = 0;
};
#pragma pack(pop)

inline uint32_t BytesPerPixel(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return 4;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
        return 8;
    default:
        return 0;
    }
}

class Harness
{
  public:
    static Harness& Get()
    {
        static Harness instance;
        return instance;
    }

    bool Enabled() const { return Config::Instance()->DlssNrHudlessTestControl.value_or_default(); }

    // Present thread, once per HUD-less frame. Cheap: a file time check every 250 ms.
    void Poll()
    {
        if (!Enabled())
            return;
        const auto now = std::chrono::steady_clock::now();
        if (now < _nextPoll)
            return;
        _nextPoll = now + std::chrono::milliseconds(250);
        const auto folder = Util::DllPath().parent_path();
        const auto control = folder / "nr-test-control.txt";
        std::error_code error;
        const auto time = std::filesystem::last_write_time(control, error);
        if (error || time == _lastControlTime)
            return;
        _lastControlTime = time;
        std::ifstream file(control);
        std::string line, seq, record, crops, directory;
        int frames = 0;
        auto& cfg = *Config::Instance();
        while (std::getline(file, line))
        {
            const auto equals = line.find('=');
            if (equals == std::string::npos)
                continue;
            const auto key = line.substr(0, equals);
            const auto value = line.substr(equals + 1);
            const auto number = [&] { return std::strtof(value.c_str(), nullptr); };
            if (key == "seq")
                seq = value;
            else if (key == "MotionAdaptive")
                cfg.DlssNrHudlessMotionAdaptive = number() != 0;
            else if (key == "StillStrength")
                cfg.DlssNrHudlessStillStrength = number();
            else if (key == "MotionStrength")
                cfg.DlssNrHudlessMotionStrength = number();
            else if (key == "MotionStart")
                cfg.DlssNrHudlessMotionStart = number();
            else if (key == "MotionFull")
                cfg.DlssNrHudlessMotionFull = number();
            else if (key == "Smoothing")
                cfg.DlssNrHudlessSmoothing = number() != 0;
            else if (key == "SmoothingFrames")
                cfg.DlssNrHudlessSmoothingFrames = number();
            else if (key == "SmoothingTolerance")
                cfg.DlssNrHudlessSmoothingTolerance = number();
            else if (key == "SmoothingReject")
                cfg.DlssNrHudlessSmoothingReject = number();
            else if (key == "SmoothingDetail")
                cfg.DlssNrHudlessSmoothingDetail = number() != 0;
            else if (key == "DebugView")
                cfg.DlssNrHudlessDebugView = (int) number();
            else if (key == "ApplyModel")
                cfg.DlssNrApplyModel = number() != 0;
            else if (key == "Enabled")
                cfg.DlssNrEnabled = number() != 0;
            else if (key == "record")
                record = value;
            else if (key == "frames")
                frames = (int) number();
            else if (key == "crops")
                crops = value;
            else if (key == "dir")
                directory = value;
        }
        if (!record.empty() && frames > 0)
            StartRecording(record, frames, crops, directory.empty() ? folder / "nr-test-recordings" : directory);
        Ack("seq=" + seq + " applied");
    }

    bool Recording() const { return _remaining > 0 && !_failed; }

    // Called with the frame's command list, after the adjusted output is written and before it is copied back.
    // Every source is returned to the state it came in.
    bool Record(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, Source before, Source nr, Source output,
                Source motion, const FrameInfo& info)
    {
        if (!Recording() || !cmd || !device || !before.resource || !nr.resource || !output.resource ||
            !motion.resource)
            return false;
        std::lock_guard lock(_mutex);
        for (auto& entry : _ring)
            if (entry.state == Entry::Recorded)
                entry.state = Entry::Free; // recorded but never submitted
        _current = nullptr;
        if (!_prepared && !Prepare(device, before.resource, motion.resource, info))
            return false;
        if (info.outputWidth != _outputWidth || info.outputHeight != _outputHeight ||
            info.guideWidth != _guideWidth || info.guideHeight != _guideHeight)
        {
            Fail("the picture or motion vector size changed while recording");
            return false;
        }
        Entry* entry = nullptr;
        for (auto& e : _ring)
            if (e.state == Entry::Free)
            {
                entry = &e;
                break;
            }
        if (!entry)
        {
            ++_dropped; // the writer is behind; skip this frame rather than wait
            return false;
        }
        const Source colours[3] = { before, nr, output };
        auto transition = [&](const Source& s, bool toCopy)
        {
            if (s.state == D3D12_RESOURCE_STATE_COPY_SOURCE)
                return;
            D3D12_RESOURCE_BARRIER b {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = s.resource;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = toCopy ? s.state : D3D12_RESOURCE_STATE_COPY_SOURCE;
            b.Transition.StateAfter = toCopy ? D3D12_RESOURCE_STATE_COPY_SOURCE : s.state;
            cmd->ResourceBarrier(1, &b);
        };
        std::vector<ID3D12Resource*> moved;
        auto toCopy = [&](const Source& s)
        {
            for (auto* r : moved)
                if (r == s.resource)
                    return;
            moved.push_back(s.resource);
            transition(s, true);
        };
        for (auto& s : colours)
            toCopy(s);
        toCopy(motion);
        size_t block = 0;
        for (size_t c = 0; c < _crops.size(); ++c)
        {
            for (int k = 0; k < 3; ++k)
                CopyBlock(cmd, entry->buffer.Get(), colours[k].resource, _crops[c], _layouts[block++]);
            CopyBlock(cmd, entry->buffer.Get(), motion.resource, _guideCrops[c], _layouts[block++]);
        }
        moved.clear();
        auto back = [&](const Source& s)
        {
            for (auto* r : moved)
                if (r == s.resource)
                    return;
            moved.push_back(s.resource);
            transition(s, false);
        };
        for (auto& s : colours)
            back(s);
        back(motion);
        entry->header = {};
        entry->header.index = _recorded;
        entry->header.gameFrame = info.gameFrame;
        entry->header.milliseconds = info.milliseconds;
        entry->header.mvScaleX = info.mvScaleX;
        entry->header.mvScaleY = info.mvScaleY;
        entry->header.outputWidth = info.outputWidth;
        entry->header.outputHeight = info.outputHeight;
        entry->header.guideX = info.guideX;
        entry->header.guideY = info.guideY;
        entry->header.guideWidth = info.guideWidth;
        entry->header.guideHeight = info.guideHeight;
        entry->header.reset = info.reset;
        entry->header.historyValid = info.historyValid;
        entry->header.temporal = info.temporal;
        entry->header.adaptive = info.adaptive;
        entry->header.smoothing = info.smoothing;
        entry->header.stillStrength = info.stillStrength;
        entry->header.motionStrength = info.motionStrength;
        entry->header.motionStart = info.motionStart;
        entry->header.motionFull = info.motionFull;
        entry->header.frames = info.frames;
        entry->header.tolerance = info.tolerance;
        entry->header.reject = info.reject;
        entry->header.bandSplit = info.bandSplit;
        entry->state = Entry::Recorded;
        _current = entry;
        return true;
    }

    // After the frame's command list was submitted and the fence signalled.
    void Commit(ID3D12Fence* fence, uint64_t value)
    {
        std::lock_guard lock(_mutex);
        if (!_current)
            return;
        _current->fence = fence;
        _current->value = value;
        _current->state = Entry::Submitted;
        _queue.push_back(_current);
        _current = nullptr;
        ++_recorded;
        if (--_remaining <= 0)
            _remaining = 0;
        _wake.notify_one();
    }

  private:
    struct Entry
    {
        enum State
        {
            Free,
            Recorded,
            Submitted
        } state = Free;
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        const uint8_t* mapped = nullptr;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        uint64_t value = 0;
        FrameHeader header;
    };
    struct Layout
    {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
        uint32_t rowBytes = 0, rows = 0;
    };

    Harness() = default;

    void Ack(const std::string& text)
    {
        std::lock_guard lock(_ackMutex);
        std::ofstream(Util::DllPath().parent_path() / "nr-test-control.ack", std::ios::app) << text << '\n';
    }

    void Fail(const std::string& reason)
    {
        _failed = true;
        _remaining = 0;
        LOG_WARN("HUD-less test recorder: {}", reason);
        Ack("record_failed=" + _name + " reason=" + reason);
    }

    void StartRecording(const std::string& name, int frames, const std::string& crops,
                        const std::filesystem::path& directory)
    {
        std::lock_guard lock(_mutex);
        if (_remaining > 0 || !_queue.empty() || _writing)
        {
            Ack("record_busy=" + name);
            return;
        }
        _crops.clear();
        std::stringstream list(crops);
        std::string item;
        while (std::getline(list, item, ';'))
        {
            Rect r;
            if (std::sscanf(item.c_str(), "%d,%d,%d,%d", &r.x, &r.y, &r.w, &r.h) == 4 && r.w > 0 && r.h > 0)
                _crops.push_back(r);
        }
        if (_crops.empty())
        {
            Ack("record_failed=" + name + " reason=no crops");
            return;
        }
        _name = name;
        _directory = directory;
        _remaining = frames;
        _recorded = 0;
        _written = 0;
        _dropped = 0;
        _failed = false;
        _prepared = false;
        _ring.clear();
        if (!_writer.joinable())
            _writer = std::thread([this] { WriterLoop(); });
        LOG_INFO("HUD-less test recorder: '{}' {} frames, {} crops", name, frames, _crops.size());
    }

    bool Prepare(ID3D12Device* device, ID3D12Resource* colour, ID3D12Resource* motion, const FrameInfo& info)
    {
        const auto colourDesc = colour->GetDesc();
        const auto motionDesc = motion->GetDesc();
        _colourBytes = BytesPerPixel(colourDesc.Format);
        _motionBytes = BytesPerPixel(motionDesc.Format);
        if (!_colourBytes || !_motionBytes)
        {
            Fail("unsupported formats " + std::to_string(colourDesc.Format) + "/" +
                 std::to_string(motionDesc.Format));
            return false;
        }
        _outputWidth = info.outputWidth;
        _outputHeight = info.outputHeight;
        _guideWidth = info.guideWidth;
        _guideHeight = info.guideHeight;
        _guideCrops.clear();
        _layouts.clear();
        uint64_t total = 0;
        auto add = [&](const D3D12_RESOURCE_DESC& source, const Rect& r)
        {
            auto desc = source;
            desc.Width = (UINT64) r.w;
            desc.Height = (UINT) r.h;
            desc.MipLevels = 1;
            desc.DepthOrArraySize = 1;
            desc.Flags = D3D12_RESOURCE_FLAG_NONE;
            Layout layout;
            UINT64 bytes = 0;
            device->GetCopyableFootprints(&desc, 0, 1, total, &layout.footprint, &layout.rows, nullptr, &bytes);
            layout.rowBytes = (uint32_t) (r.w * BytesPerPixel(source.Format));
            total = (layout.footprint.Offset + bytes + 511) & ~uint64_t(511);
            _layouts.push_back(layout);
        };
        for (auto& r : _crops)
        {
            r.x = std::clamp(r.x, 0, (int) info.outputWidth - 1);
            r.y = std::clamp(r.y, 0, (int) info.outputHeight - 1);
            r.w = std::min(r.w, (int) info.outputWidth - r.x);
            r.h = std::min(r.h, (int) info.outputHeight - r.y);
            Rect g;
            const double sx = double(info.guideWidth) / info.outputWidth;
            const double sy = double(info.guideHeight) / info.outputHeight;
            g.x = std::max(0, (int) std::floor(r.x * sx) - 2);
            g.y = std::max(0, (int) std::floor(r.y * sy) - 2);
            g.w = std::min((int) info.guideWidth - g.x, (int) std::ceil(r.w * sx) + 4);
            g.h = std::min((int) info.guideHeight - g.y, (int) std::ceil(r.h * sy) + 4);
            g.x += info.guideX;
            g.y += info.guideY;
            _guideCrops.push_back(g);
            for (int k = 0; k < 3; ++k)
                add(colourDesc, r);
            add(motionDesc, g);
        }
        _frameBytes = 0;
        for (size_t c = 0; c < _crops.size(); ++c)
            _frameBytes += 3ull * _crops[c].w * _crops[c].h * _colourBytes +
                           uint64_t(_guideCrops[c].w) * _guideCrops[c].h * _motionBytes;
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = total;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        _ring.resize(12);
        for (auto& e : _ring)
        {
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&e.buffer))))
            {
                Fail("readback allocation failed");
                return false;
            }
            void* data = nullptr;
            D3D12_RANGE range { 0, 0 };
            e.buffer->Map(0, nullptr, &data);
            e.mapped = static_cast<const uint8_t*>(data);
        }
        std::error_code error;
        std::filesystem::create_directories(_directory, error);
        _file = std::fopen((_directory / (_name + ".bin")).string().c_str(), "wb");
        if (!_file)
        {
            Fail("cannot write " + (_directory / (_name + ".bin")).string());
            return false;
        }
        std::ofstream meta(_directory / (_name + ".json"));
        meta << "{\n  \"name\": \"" << _name << "\",\n  \"colour_format\": " << colourDesc.Format
             << ",\n  \"motion_format\": " << motionDesc.Format << ",\n  \"colour_bytes\": " << _colourBytes
             << ",\n  \"motion_bytes\": " << _motionBytes << ",\n  \"header_bytes\": " << sizeof(FrameHeader)
             << ",\n  \"frame_bytes\": " << _frameBytes << ",\n  \"output\": [" << info.outputWidth << ", "
             << info.outputHeight << "],\n  \"guide\": [" << info.guideX << ", " << info.guideY << ", "
             << info.guideWidth << ", " << info.guideHeight << "],\n  \"crops\": [";
        for (size_t c = 0; c < _crops.size(); ++c)
            meta << (c ? ", " : "") << "{\"rect\": [" << _crops[c].x << ", " << _crops[c].y << ", " << _crops[c].w
                 << ", " << _crops[c].h << "], \"guide\": [" << _guideCrops[c].x << ", " << _guideCrops[c].y << ", "
                 << _guideCrops[c].w << ", " << _guideCrops[c].h << "]}";
        meta << "]\n}\n";
        _prepared = true;
        return true;
    }

    void CopyBlock(ID3D12GraphicsCommandList* cmd, ID3D12Resource* buffer, ID3D12Resource* source, const Rect& r,
                   const Layout& layout)
    {
        D3D12_TEXTURE_COPY_LOCATION to {};
        to.pResource = buffer;
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint = layout.footprint;
        D3D12_TEXTURE_COPY_LOCATION from {};
        from.pResource = source;
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.SubresourceIndex = 0;
        D3D12_BOX box { (UINT) r.x, (UINT) r.y, 0, (UINT) (r.x + r.w), (UINT) (r.y + r.h), 1 };
        cmd->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
    }

    void WriterLoop()
    {
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        std::vector<uint8_t> frame;
        for (;;)
        {
            Entry* entry = nullptr;
            {
                std::unique_lock lock(_mutex);
                _wake.wait_for(lock, std::chrono::milliseconds(200), [&] { return !_queue.empty(); });
                if (_queue.empty())
                {
                    if ((_failed || _remaining == 0) && _file)
                        Finish();
                    continue;
                }
                entry = _queue.front();
                _writing = true;
            }
            // The GPU copy and the disk write happen without the lock, so the present thread never waits on them.
            bool lost = false;
            while (!lost && entry->fence->GetCompletedValue() < entry->value)
            {
                entry->fence->SetEventOnCompletion(entry->value, event);
                WaitForSingleObject(event, 100);
                lost = entry->fence->GetCompletedValue() == UINT64_MAX;
            }
            lost = lost || entry->fence->GetCompletedValue() == UINT64_MAX;
            if (!lost && _file)
            {
                frame.clear();
                const auto* header = reinterpret_cast<const uint8_t*>(&entry->header);
                frame.insert(frame.end(), header, header + sizeof(FrameHeader));
                for (auto& layout : _layouts)
                {
                    const uint8_t* base = entry->mapped + layout.footprint.Offset;
                    for (uint32_t row = 0; row < layout.rows; ++row)
                    {
                        const uint8_t* line = base + uint64_t(row) * layout.footprint.Footprint.RowPitch;
                        frame.insert(frame.end(), line, line + layout.rowBytes);
                    }
                }
                std::fwrite(frame.data(), 1, frame.size(), _file);
            }
            std::lock_guard lock(_mutex);
            _queue.pop_front();
            entry->state = Entry::Free;
            _writing = false;
            if (lost)
                Fail("device removed");
            else
                ++_written;
            if ((_remaining == 0 || _failed) && _queue.empty() && _file)
                Finish();
        }
    }

    void Finish()
    {
        std::fclose(_file);
        _file = nullptr;
        if (_failed)
            return;
        Ack("record_done=" + _name + " written=" + std::to_string(_written) + " dropped=" + std::to_string(_dropped));
        LOG_INFO("HUD-less test recorder: '{}' done, {} frames written, {} dropped", _name, _written, _dropped);
    }

    std::chrono::steady_clock::time_point _nextPoll {};
    std::filesystem::file_time_type _lastControlTime {};
    std::mutex _mutex, _ackMutex;
    std::condition_variable _wake;
    std::thread _writer;
    std::vector<Entry> _ring;
    std::deque<Entry*> _queue;
    Entry* _current = nullptr;
    std::vector<Rect> _crops, _guideCrops;
    std::vector<Layout> _layouts;
    std::string _name;
    std::filesystem::path _directory;
    std::FILE* _file = nullptr;
    std::atomic<int> _remaining { 0 };
    std::atomic<bool> _failed { false };
    bool _prepared = false;
    bool _writing = false;
    uint32_t _recorded = 0, _written = 0, _dropped = 0;
    uint32_t _colourBytes = 0, _motionBytes = 0;
    uint32_t _outputWidth = 0, _outputHeight = 0, _guideWidth = 0, _guideHeight = 0;
    uint64_t _frameBytes = 0;
};
} // namespace DlssNr::HudlessTest
