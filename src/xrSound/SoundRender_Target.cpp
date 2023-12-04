#include "stdafx.h"

#include "SoundRender_Target.h"
#include "SoundRender_Core.h"
#include "SoundRender_Emitter.h"
#include "SoundRender_Source.h"
#include "xrCore/Threading/TaskManager.hpp"

CSoundRender_Target::CSoundRender_Target()
{
    buffers_to_prefill.reserve(sdef_target_count);
}

void CSoundRender_Target::_destroy()
{
    wait_prefill();
}

void CSoundRender_Target::start(CSoundRender_Emitter* E)
{
    R_ASSERT(E);

    // *** Initial buffer startup ***
    // 1. Fill parameters
    // 4. Load 2 blocks of data (as much as possible)
    // 5. Deferred-play-signal (emitter-exist, rendering-false)
    m_pEmitter = E;
    rendering = false;
    //m_pEmitter->source()->attach();

    // Calc storage
    for (auto& buf : temp_buf)
        buf.resize(E->source()->m_info.bytesPerBuffer);

#ifdef USE_PHONON
    if (const auto context = SoundRender->ipl_context())
    {
        const auto& info = E->source()->m_info;
        auto& settings = E->source()->ipl_audio_settings;

        IPLDirectEffectSettings direct{ info.channels };
        iplDirectEffectCreate(context, &settings, &direct, &ipl_effects.direct);

        IPLReflectionEffectSettings refl{ IPL_REFLECTIONEFFECTTYPE_CONVOLUTION, settings.frameSize * 2, 4 };
        iplReflectionEffectCreate(context, &settings, &refl, &ipl_effects.reflection);

        IPLPathEffectSettings path{ 1, IPL_TRUE, {}, SoundRender->ipl_hrtf() };
        iplPathEffectCreate(context, &settings, &path, &ipl_effects.path);

        iplAudioBufferAllocate(context, info.channels, settings.frameSize, &ipl_buffers.direct_input);
        iplAudioBufferAllocate(context, info.channels, settings.frameSize, &ipl_buffers.direct_output);
    }
#endif
}

void CSoundRender_Target::render()
{
    VERIFY(!rendering);
    rendering = true;
#ifdef USE_PHONON
    if (psSoundFlags.test(ss_EFX) && m_pEmitter->scene->ipl_scene_mesh() && !m_pEmitter->is_2D())
    {
        const auto context = SoundRender->ipl_context();

        IPLSimulationOutputs outputs{};
        outputs.direct.flags = static_cast<IPLDirectEffectFlags>(
            IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION |
            IPL_DIRECTEFFECTFLAGS_APPLYDIRECTIVITY |
            IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION |
            IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION
        );
        iplSourceGetOutputs(m_pEmitter->ipl_source(), IPL_SIMULATIONFLAGS_DIRECT, &outputs);

        for (auto& buf : temp_buf)
        {
            iplAudioBufferDeinterleave(context, (float*)buf.data(), &ipl_buffers.direct_input);

            iplDirectEffectApply(ipl_effects.direct, &outputs.direct, &ipl_buffers.direct_input, &ipl_buffers.direct_output);

            iplAudioBufferInterleave(context, &ipl_buffers.direct_output, (float*)buf.data());
        }
    }
#endif
}

void CSoundRender_Target::stop()
{
    wait_prefill();
    m_pEmitter->source()->detach();
    m_pEmitter = nullptr;
    rendering = false;

#ifdef USE_PHONON
    if (const auto context = SoundRender->ipl_context())
    {
        iplDirectEffectRelease(&ipl_effects.direct);
        iplReflectionEffectRelease(&ipl_effects.reflection);
        iplPathEffectRelease(&ipl_effects.path);

        iplAudioBufferFree(context, &ipl_buffers.direct_output);
        iplAudioBufferFree(context, &ipl_buffers.direct_input);
    }
#endif
}

void CSoundRender_Target::rewind()
{
    R_ASSERT(rendering);
}

void CSoundRender_Target::update()
{
    R_ASSERT(m_pEmitter);
    wait_prefill();

#ifdef USE_PHONON
    if (psSoundFlags.test(ss_EFX) && m_pEmitter->scene->ipl_scene_mesh() && !m_pEmitter->is_2D())
    {
        const auto context = SoundRender->ipl_context();

        IPLSimulationOutputs outputs{};
        outputs.direct.flags = static_cast<IPLDirectEffectFlags>(
            IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION |
            IPL_DIRECTEFFECTFLAGS_APPLYDIRECTIVITY |
            IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION |
            IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION
        );
        iplSourceGetOutputs(m_pEmitter->ipl_source(), IPL_SIMULATIONFLAGS_DIRECT, &outputs);

        for (auto& buf : temp_buf)
        {
            iplAudioBufferDeinterleave(context, (float*)buf.data(), &ipl_buffers.direct_input);

            iplDirectEffectApply(ipl_effects.direct, &outputs.direct, &ipl_buffers.direct_input, &ipl_buffers.direct_output);

            iplAudioBufferInterleave(context, &ipl_buffers.direct_output, (float*)buf.data());
        }
    }
#endif
}

void CSoundRender_Target::fill_parameters()
{
    VERIFY(m_pEmitter);
    //if (pEmitter->b2D)
    //    pEmitter->set_position(SoundRender->listener_position());
}

void CSoundRender_Target::fill_block(size_t idx)
{
    R_ASSERT(m_pEmitter);
    m_pEmitter->fill_block(temp_buf[idx].data(), temp_buf[idx].size());
}

void CSoundRender_Target::fill_all_blocks()
{
    for (size_t i = 0; i < sdef_target_count; ++i)
        fill_block(i);
}

void CSoundRender_Target::prefill_blocks(Task&, void*)
{
    for (const size_t idx : buffers_to_prefill)
        fill_block(idx);
    buffers_to_prefill.clear();
    prefill_task.store(nullptr, std::memory_order_release);
}

void CSoundRender_Target::prefill_all_blocks(Task&, void*)
{
    fill_all_blocks();
    prefill_task.store(nullptr, std::memory_order_release);
}

void CSoundRender_Target::wait_prefill() const
{
    if (const auto task = prefill_task.load(std::memory_order_relaxed))
        TaskScheduler->Wait(*task);
}

void CSoundRender_Target::dispatch_prefill()
{
    wait_prefill();
    const auto task = &TaskScheduler->AddTask("CSoundRender_Target::dispatch_prefill()", { this, &CSoundRender_Target::prefill_blocks });
    prefill_task.store(task, std::memory_order_release);
}

void CSoundRender_Target::dispatch_prefill_all()
{
    wait_prefill();
    const auto task = &TaskScheduler->AddTask("CSoundRender_Target::dispatch_prefill_all()", { this, &CSoundRender_Target::prefill_all_blocks });
    prefill_task.store(task, std::memory_order_release);
}
