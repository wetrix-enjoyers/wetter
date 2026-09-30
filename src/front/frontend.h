// front end: the display-list walker. It follows an F3DEX list through RDRAM,
// hands geometry commands to the Rsp and RDP commands to an RdpSink.
#pragma once

#include <cstdint>

#include "rdp_sink.h"
#include "rsp.h"

namespace wetter::front {

class Frontend {
public:
    // force_branch_z: take every G_BRANCH_Z instead of comparing the vertex's depth.
    // x_extent: how far past the image's edges full-width 3D reaches (widescreen).
    Frontend(uint8_t* rdram, RdpSink& rdp, bool force_branch_z, float x_extent = 1.0f);

    void set_rdram(uint8_t* rdram);

    // The RSP starts each task with empty matrix stacks (the runtime reloads the
    // microcode and DMEM before every task); see Rsp::begin_task.
    void begin_task() { rsp_.begin_task(); }

    // Walks the display list at `address` (a masked RDRAM address) to its end.
    void run(uint32_t address);

    // The window in which the command and geometry logs print.
    void set_log_window(bool open, bool geometry_log);

    Rsp& rsp() { return rsp_; }
    const Rsp& rsp() const { return rsp_; }
    uint64_t triangles_waiting() const { return triangles_waiting_; }

private:
    uint32_t resolve(uint32_t address) const;
    uint32_t read_word(uint32_t address) const;

    uint8_t* rdram_ = nullptr;
    RdpSink& rdp_;
    Rsp rsp_;
    bool force_branch_z_ = false;

    uint32_t segments_[16] = {};
    uint32_t geometry_mode_ = 0;
    uint32_t rdphalf1_ = 0;   // G_RDPHALF_1's word, for G_BRANCH_Z
    uint64_t triangles_waiting_ = 0;
    bool log_window_open_ = false;
};

}  // namespace wetter::front
