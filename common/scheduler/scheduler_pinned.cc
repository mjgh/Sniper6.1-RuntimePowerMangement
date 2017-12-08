#include "scheduler_pinned.h"
#include "config.hpp"

#include "thread.h" //Javad

SchedulerPinned::SchedulerPinned(ThreadManager *thread_manager)
    : SchedulerPinnedBase(thread_manager, SubsecondTime::NS(Sim()->getCfg()->getInt("scheduler/pinned/quantum")))
    , m_interleaving(Sim()->getCfg()->getInt("scheduler/pinned/interleaving"))
    , m_next_core(0)
{
    m_core_mask.resize(Sim()->getConfig()->getApplicationCores());

    for (core_id_t core_id = 0; core_id < (core_id_t)Sim()->getConfig()->getApplicationCores(); core_id++)
    {
        m_core_mask[core_id] = Sim()->getCfg()->getBoolArray("scheduler/pinned/core_mask", core_id);
    }
}

core_id_t SchedulerPinned::getNextCore(core_id_t core_id)
{
    while (true)
    {
        core_id += m_interleaving;
        if (core_id >= (core_id_t)Sim()->getConfig()->getApplicationCores())
        {
            core_id %= Sim()->getConfig()->getApplicationCores();
            core_id += 1;
            core_id %= m_interleaving;
        }
        if (m_core_mask[core_id])
            return core_id;
    }
}

core_id_t SchedulerPinned::getFreeCore(core_id_t core_first)
{
    core_id_t core_next = core_first;

    do
    {
        if (m_core_thread_running[core_next] == INVALID_THREAD_ID)
            return core_next;

        core_next = getNextCore(core_next);
    } while (core_next != core_first);

    return core_first;
}

void SchedulerPinned::threadSetInitialAffinity(thread_id_t thread_id)
{
    app_id_t app_id = Sim()->getThreadManager()->getThreadFromID(thread_id)->getAppId();

    if (app_id == 0)
        if (thread_id == 0)
            m_thread_info[thread_id].setAffinitySingle(0);
        else
        {
            cpu_set_t MyMask;
            CPU_ZERO(&MyMask);

            threadSetAffinity(INVALID_THREAD_ID, thread_id, sizeof(MyMask), &MyMask);
        }
}
