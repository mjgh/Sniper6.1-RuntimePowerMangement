#include "scheduler_pinned_base.h"
#include "simulator.h"
#include "core_manager.h"
#include "performance_model.h"
#include "os_compat.h"

#include <sstream>

#include "thread.h" 
#include "magic_server.h"

#include "dvfs_manager.h"
#include <iostream>
#include <dirent.h>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <time.h>

using namespace std;

// Pinned scheduler.
// Each thread has is pinned to a specific core (m_thread_affinity).
// Cores are handed out to new threads in round-robin fashion.
// If multiple threads share a core, they are time-shared with a configurable quantum

SchedulerPinnedBase::SchedulerPinnedBase(ThreadManager *thread_manager, SubsecondTime quantum)
    : SchedulerDynamic(thread_manager), m_quantum(quantum), m_last_periodic(SubsecondTime::Zero()), m_core_thread_running(Sim()->getConfig()->getApplicationCores(), INVALID_THREAD_ID), m_quantum_left(Sim()->getConfig()->getApplicationCores(), SubsecondTime::Zero())
{
      /*Initialization Section*/
      MyPower = 0.0;
      MyPowerThreshold = 310; 
      MyLastPower = 0.0;
      MyAppsCount = MY_APPS_COUNT;

      MyDvfsEnabled = true;      
      MyMigrationEnabled = true; 

      for (int i = 0; i < MyAppsCount; i++)
      {
            MyIsBig[i] = MyAppsKickPriority[i] = -1;
            MyPowerBlackList[i] = MyHasStarted[i] = MyHasEnded[i] = 0;
            first_timex[i] = true;

            MyQosValues[i] = 500;
            MyCurrentAppFreq[i] = Sim()->getMagicServer()->getFrequency(i);

            if (i == 0)
                  start_cyclesx[i] = 0;
            else if (i == 1)
                  start_cyclesx[i] = 0;
            else if (i == 2)
                  start_cyclesx[i] = 0;
            else if (i == 3)
                  start_cyclesx[i] = 0;
            else if (i == 4)
                  start_cyclesx[i] = 0;
            else if (i == 5)
                  start_cyclesx[i] = 0;
      }
      blacklist_candidate = -1;

      MyMaxCoreFreq = Sim()->getMagicServer()->getFrequency(0);
      MyFreqDecStep = 500;
}

core_id_t SchedulerPinnedBase::findFreeCoreForThread(thread_id_t thread_id)
{
      for (core_id_t core_id = 0; core_id < (core_id_t)Sim()->getConfig()->getApplicationCores(); ++core_id)
      {
            if (m_thread_info[thread_id].hasAffinity(core_id) && m_core_thread_running[core_id] == INVALID_THREAD_ID)
            {
                  return core_id;
            }
      }
      return INVALID_CORE_ID;
}

core_id_t SchedulerPinnedBase::threadCreate(thread_id_t thread_id)
{
      if (m_thread_info.size() <= (size_t)thread_id)
            m_thread_info.resize(m_thread_info.size() + 16);

      if (m_thread_info[thread_id].hasAffinity())
      {
            // Thread already has an affinity set at/before creation
      }
      else
      {
            threadSetInitialAffinity(thread_id);
      }

      // The first thread scheduled on this core can start immediately, the others have to wait
      core_id_t free_core_id = findFreeCoreForThread(thread_id);
      if (free_core_id != INVALID_CORE_ID)
      {
            m_thread_info[thread_id].setCoreRunning(free_core_id);
            m_core_thread_running[free_core_id] = thread_id;
            m_quantum_left[free_core_id] = m_quantum;
            return free_core_id;
      }
      else
      {
            m_thread_info[thread_id].setCoreRunning(INVALID_CORE_ID);
            return INVALID_CORE_ID;
      }
}

void SchedulerPinnedBase::threadYield(thread_id_t thread_id)
{
      core_id_t core_id = m_thread_info[thread_id].getCoreRunning();

      if (core_id != INVALID_CORE_ID)
      {
            Core *core = Sim()->getCoreManager()->getCoreFromID(core_id);
            SubsecondTime time = core->getPerformanceModel()->getElapsedTime();

            m_quantum_left[core_id] = SubsecondTime::Zero();
            reschedule(time, core_id, false);

            if (!m_thread_info[thread_id].hasAffinity(core_id))
            {
                  core_id_t free_core_id = findFreeCoreForThread(thread_id);
                  if (free_core_id != INVALID_CORE_ID)
                  {
                        // We have just been moved to a different core(s), and one of them is free. Schedule us there now.
                        reschedule(time, free_core_id, false);
                  }
            }
      }
}

bool SchedulerPinnedBase::threadSetAffinity(thread_id_t calling_thread_id, thread_id_t thread_id, size_t cpusetsize, const cpu_set_t *mask)
{
      if (m_thread_info.size() <= (size_t)thread_id)
            m_thread_info.resize(thread_id + 16);

      m_thread_info[thread_id].setExplicitAffinity();

      if (!mask)
      {
            // No mask given: free to schedule anywhere.
            for (core_id_t core_id = 0; core_id < (core_id_t)Sim()->getConfig()->getApplicationCores(); ++core_id)
            {
                  m_thread_info[thread_id].addAffinity(core_id);
            }
      }
      else
      {
            m_thread_info[thread_id].clearAffinity();

            for (unsigned int cpu = 0; cpu < 8 * cpusetsize; ++cpu)
            {
                  if (CPU_ISSET_S(cpu, cpusetsize, mask))
                  {
                        LOG_ASSERT_ERROR(cpu < Sim()->getConfig()->getApplicationCores(), "Invalid core %d found in sched_setaffinity() mask", cpu);

                        m_thread_info[thread_id].addAffinity(cpu);
                  }
            }
      }

      // We're setting the affinity of a thread that isn't yet created. Do nothing else for now.
      if (thread_id >= (thread_id_t)Sim()->getThreadManager()->getNumThreads())
            return true;

      if (thread_id == calling_thread_id)
      {
            threadYield(thread_id);
      }
      else if (m_thread_info[thread_id].isRunning()                                                 // Thread is running
               && !m_thread_info[thread_id].hasAffinity(m_thread_info[thread_id].getCoreRunning())) // but not where we want it to
      {
            // Reschedule the thread as soon as possible
            m_quantum_left[m_thread_info[thread_id].getCoreRunning()] = SubsecondTime::Zero();
      }
      else if (m_threads_runnable[thread_id]             // Thread is runnable
               && !m_thread_info[thread_id].isRunning()) // Thread is not running (we can't preempt it outside of the barrier)
      {
            core_id_t free_core_id = findFreeCoreForThread(thread_id);
            if (free_core_id != INVALID_THREAD_ID) // Thread's new core is free
            {
                  // We have just been moved to a different core, and that core is free. Schedule us there now.
                  Core *core = Sim()->getCoreManager()->getCoreFromID(free_core_id);
                  SubsecondTime time = std::max(core->getPerformanceModel()->getElapsedTime(), Sim()->getClockSkewMinimizationServer()->getGlobalTime());
                  reschedule(time, free_core_id, false);
            }
      }

      return true;
}

bool SchedulerPinnedBase::threadGetAffinity(thread_id_t thread_id, size_t cpusetsize, cpu_set_t *mask)
{
      if (cpusetsize * 8 < Sim()->getConfig()->getApplicationCores())
      {
            // Not enough space to return result
            return false;
      }

      CPU_ZERO_S(cpusetsize, mask);
      for (core_id_t core_id = 0; core_id < (core_id_t)Sim()->getConfig()->getApplicationCores(); ++core_id)
      {
            if (
                m_thread_info[thread_id].hasAffinity(core_id)
                // When application has not yet done any sched_setaffinity calls, lie and return a fully populated affinity bitset.
                // This makes libiomp5 use all available cores.
                || !m_thread_info[thread_id].hasExplicitAffinity())
                  CPU_SET_S(core_id, cpusetsize, mask);
      }

      return true;
}

void SchedulerPinnedBase::threadStart(thread_id_t thread_id, SubsecondTime time)
{
      // Thread transitioned out of INITIALIZING, if it did not get a core assigned by threadCreate but there is a free one now, schedule it there
      core_id_t free_core_id = findFreeCoreForThread(thread_id);
      if (free_core_id != INVALID_THREAD_ID)
            reschedule(time, free_core_id, false);
}

void SchedulerPinnedBase::threadStall(thread_id_t thread_id, ThreadManager::stall_type_t reason, SubsecondTime time)
{
      // If the running thread becomes unrunnable, schedule someone else
      if (m_thread_info[thread_id].isRunning())
            reschedule(time, m_thread_info[thread_id].getCoreRunning(), false);
}

void SchedulerPinnedBase::threadResume(thread_id_t thread_id, thread_id_t thread_by, SubsecondTime time)
{
      // If our core is currently idle, schedule us now
      core_id_t free_core_id = findFreeCoreForThread(thread_id);
      if (free_core_id != INVALID_THREAD_ID)
            reschedule(time, free_core_id, false);
}

void SchedulerPinnedBase::threadExit(thread_id_t thread_id, SubsecondTime time)
{
      // If the running thread becomes unrunnable, schedule someone else
      if (m_thread_info[thread_id].isRunning())
            reschedule(time, m_thread_info[thread_id].getCoreRunning(), false);
}

/////////////////////////Modifications
void SchedulerPinnedBase::MyUpdatePower(void)
{

      time_t rawtime;
      struct tm *timeinfo;

      char path[300] = "/home/mohgha/Thesis/benchmarks/ali4/energystats-temp.txt"; //file address
      char *contents;                                                              //contains the first 2000 charecters of text file

      ifstream file(path, ios::in | ios::binary | ios::ate);
      if (file.is_open())
      {
            file.seekg(0, ios::end);
            int size = 2000; 
            contents = new char[size];
            file.seekg(0, ios::beg);
            file.read(contents, size); //load contents as much as size
            file.close();
      }
      else
            return;
      
      // To read Peak Power

      char *str = strstr(contents, "Peak Power = "), *p;
      double peak_power = 0.0;

      if (!str)
      {
            delete[] contents;
            return;
      }
      if (!strstr(str, "Total Leakage = "))
      {
            delete[] contents;
            return;
      }
      //

      p = str;

      while (*p)
      {
            if (isdigit(*p))
            {
                  peak_power = strtod(p, &p);
                  break;
            }
            else
                  p++;
      }

      if (MyPower != peak_power)
      {
            time(&rawtime);
            timeinfo = localtime(&rawtime);

            MyPower = peak_power;
            printf("\n%s %f\n", asctime(timeinfo), MyPower);

            //
            ofstream file2("/home/mohgha/Thesis/benchmarks/ali4/results.txt", ios::out | ios::binary | ios::app);
            if (file2.is_open())
            {
                  file2 << MyPower << endl;
                  file2.close();
            }
            else
                  return;
            //
      }

      delete[] contents;
}
//////////////////////////...
void SchedulerPinnedBase::MyThreadsStateManager() //manage state of threads - mange mapping of threads onto cores
{
      app_id_t app_id;
      core_id_t core_id;
      int thread_idx[MY_APPS_COUNT];

      for (int i = 0; i < MyAppsCount; i++)
            thread_idx[i] = 0;

      Core *core = Sim()->getCoreManager()->getCoreFromID(0);
      SubsecondTime cycles_fs = core->getPerformanceModel()->getElapsedTime();
      const ComponentPeriod *dom_global = Sim()->getDvfsManager()->getGlobalDomain();
      UInt64 cycles = SubsecondTime::divideRounded(cycles_fs, *dom_global);

      for (thread_id_t thread_id = 0; thread_id < (thread_id_t)Sim()->getThreadManager()->getNumThreads(); ++thread_id)
      {
            app_id = Sim()->getThreadManager()->getThreadFromID(thread_id)->getAppId();

            if (cycles < start_cyclesx[app_id])
            {
                  cpu_set_t MyMask;
                  CPU_ZERO(&MyMask);

                  thread_idx[app_id]++;
                  threadSetAffinity(INVALID_THREAD_ID, thread_id, sizeof(MyMask), &MyMask);
            }
            else if (first_timex[app_id])
            {
                  if (app_id == 0) //setting an arbitrary frequency limit on all cores
                  {                
                        if (Sim()->getConfig()->getApplicationCores() == 1)
                              Sim()->getMagicServer()->setFrequency(0, 2660); //to change the frequency of core0
                                                                              
                        else
                        {
                              
                              for (int i = 0; i < 32; i++)
                              {
                                    Sim()->getMagicServer()->setFrequency(i, 2660);
                              }
                        }
                        for (int i = 0; i < MyAppsCount; i++)
                              MyCurrentAppFreq[i] = Sim()->getMagicServer()->getFrequency(i);

                        MyMaxCoreFreq = Sim()->getMagicServer()->getFrequency(0);
                        
                  }
                  //
                  if (app_id == 0)
                        core_id = 0; 

                  else if (app_id == 1)
                        core_id = 2;

                  else if (app_id == 2)
                        core_id = 4;

                  else if (app_id == 3)
                        core_id = 6;

                  else if (app_id == 4)
                        core_id = 24;

                  else if (app_id == 5)
                        core_id = 28;
                  //

                  MyIsBig[app_id] = 1;
                  MyPowerBlackList[app_id] = 0;

                  MyAppsKickPriority[app_id] = -1;
                  for (int i = 0; i < MyAppsCount; i++) //to set the pririty of a newcomer app
                        if (MyAppsKickPriority[app_id] < MyAppsKickPriority[i])
                              MyAppsKickPriority[app_id] = MyAppsKickPriority[i] + 1;
                  if (MyAppsKickPriority[app_id] == -1)
                        MyAppsKickPriority[app_id] = 0;

                  MyHasStarted[app_id] = 1;

                  thread_idx[app_id]++;
                  m_thread_info[thread_id].setAffinitySingle(core_id);

                  first_timex[app_id] = false;

            }

            else
            {
                  //
                  if (app_id == 0) //Big Cores
                  {
                        switch (thread_idx[app_id])
                        {
                        case 1:
                              core_id = 1;
                              break;
                        case 2:
                              core_id = 8;
                              break;
                        case 3:
                              core_id = 9;
                              break;
                        case 4:
                              core_id = 10;
                              break;
                        case 5:
                              core_id = 16;
                              break;
                        case 6:
                              core_id = 17;
                              break;
                        case 7:
                              core_id = 18;
                              break;
                        default:
                              core_id = 0;
                        }
                  }

                  else if (app_id == 1)
                  {
                        switch (thread_idx[app_id])
                        {
                        case 1:
                              core_id = 3;
                              break;
                        default:
                              core_id = 2;
                        }
                  }

                  else if (app_id == 2)
                  {
                        switch (thread_idx[app_id])
                        {
                        case 1:
                              core_id = 5;
                              break;
                        case 2:
                              core_id = 11;
                              break;
                        case 3:
                              core_id = 12;
                              break;
                        case 4:
                              core_id = 13;
                              break;
                        case 5:
                              core_id = 19;
                              break;
                        case 6:
                              core_id = 20;
                              break;
                        case 7:
                              core_id = 21;
                              break;
                        default:
                              core_id = 4;
                        }
                  }

                  else if (app_id == 3)
                  {
                        switch (thread_idx[app_id])
                        {
                        case 1:
                              core_id = 7;
                              break;
                        case 2:
                              core_id = 14;
                              break;
                        case 3:
                              core_id = 15;
                              break;
                        case 4:
                              core_id = 22;
                              break;
                        case 5:
                              core_id = 23;
                              break;
                        case 6:
                              core_id = 30;
                              break;
                        case 7:
                              core_id = 31;
                              break;
                        default:
                              core_id = 6;
                        }
                  }

                  else if (app_id == 4)
                  {
                        switch (thread_idx[app_id])
                        {
                        case 1:
                              core_id = 25;
                              break;
                        case 2:
                              core_id = 26;
                              break;
                        case 3:
                              core_id = 27;
                              break;
                        default:
                              core_id = 24;
                        }
                  }

                  else if (app_id == 5)
                  {
                        switch (thread_idx[app_id])
                        {
                        case 1:
                              core_id = 29;
                              break;
                        default:
                              core_id = 28;
                        }
                  }
                  //

                  if (MyIsBig[app_id] == 0)
                        core_id += (Sim()->getConfig()->getApplicationCores() / 2);

                  thread_idx[app_id]++;
                  m_thread_info[thread_id].setAffinitySingle(core_id);

                  if (MyIsBig[app_id] == 1 && Sim()->getMagicServer()->getFrequency(core_id) != MyCurrentAppFreq[app_id]) //maybe the frequency of coming thread is different from the previous ones
                        Sim()->getMagicServer()->setFrequency(core_id, MyCurrentAppFreq[app_id]);
            }
      }
}

void SchedulerPinnedBase::MyAppsDeparture() //used when app leaving the system -- doing clean up job
{
      Core::State master_thread_state;

      for (int mt_id = 0; mt_id < MyAppsCount; mt_id++)
      {
            master_thread_state = Sim()->getThreadManager()->getThreadState(mt_id);

            if (master_thread_state == Core::IDLE && master_thread_state != Core::INITIALIZING && MyHasEnded[mt_id] == 0)
            {
                  blacklist_candidate = -1;
                  for (int i = 0; i < MyAppsCount; i++)
                        MyPowerBlackList[i] = 0;

                  MyIsBig[mt_id] = -1; //since this app left the system or is not started yet
                  MyAppsKickPriority[mt_id] = -1;
                  MyHasEnded[mt_id] = 1;
            }
      }
}

void SchedulerPinnedBase::MyPowerEvents(SubsecondTime time)
{
      app_id_t app_id;
      static bool moved_app_to_big;

      if (MyPower != MyLastPower)
      {
            int to_be_kicked_app = -1;
            int to_be_dvfsed_app = -1;

            for (int i = 0; i < MyAppsCount; i++) //find the app with the biggest kick priority
            {
                  if (MyAppsKickPriority[i] == -1)
                        continue;

                  if (to_be_kicked_app == -1)
                        to_be_kicked_app = i;

                  else if (MyAppsKickPriority[i] > MyAppsKickPriority[to_be_kicked_app])
                        to_be_kicked_app = i;

                  if (to_be_dvfsed_app == -1)
                  {
                        if (MyCurrentAppFreq[i] > MyQosValues[i])
                              to_be_dvfsed_app = i;
                  }

                  else if (MyAppsKickPriority[i] > MyAppsKickPriority[to_be_dvfsed_app]) //to find the dvfs candidate with the highest kick priority
                  {
                        if (MyCurrentAppFreq[i] > MyQosValues[i])
                              to_be_dvfsed_app = i;
                  }
            }

            if (MyPower > MyPowerThreshold)
            {
                  
                  if (MyDvfsEnabled == true && to_be_dvfsed_app != -1)
                  {
                        if (MyCurrentAppFreq[to_be_dvfsed_app] - MyFreqDecStep >= MyQosValues[to_be_dvfsed_app])
                              MyCurrentAppFreq[to_be_dvfsed_app] -= MyFreqDecStep;
                        else
                              MyCurrentAppFreq[to_be_dvfsed_app] = MyQosValues[to_be_dvfsed_app];

                        for (thread_id_t thread_id = 0; thread_id < (thread_id_t)Sim()->getThreadManager()->getNumThreads(); ++thread_id)
                        {
                              app_id = Sim()->getThreadManager()->getThreadFromID(thread_id)->getAppId();

                              if (app_id == to_be_dvfsed_app)
                              {
                                    core_id_t core_id;
                                    for (core_id = 0; !(m_thread_info[thread_id].hasAffinity(core_id)); ++core_id)
                                          ; //to find out which core the thread has affinity with

                                    Sim()->getMagicServer()->setFrequency(core_id, MyCurrentAppFreq[to_be_dvfsed_app]);
                              }
                        }
                  }

                  
                  else if (MyMigrationEnabled == true && to_be_kicked_app != -1) //if there is no dvfs candidate, go with migration
                  {
                        blacklist_candidate = to_be_kicked_app;
                        MyAppsKickPriority[to_be_kicked_app] = -1;
                        MyIsBig[to_be_kicked_app] = 0;
                        printf("\nMoving App %d to Small cores\n", to_be_kicked_app);

                        for (thread_id_t thread_id = 0; thread_id < (thread_id_t)Sim()->getThreadManager()->getNumThreads(); ++thread_id)
                        {
                              app_id = Sim()->getThreadManager()->getThreadFromID(thread_id)->getAppId();

                              if (app_id == to_be_kicked_app)
                              {
                                    core_id_t core_id;
                                    for (core_id = 0; !(m_thread_info[thread_id].hasAffinity(core_id)); ++core_id)
                                          ; //to find out which core the thread has affinity with

                                    m_thread_info[thread_id].setAffinitySingle((Sim()->getConfig()->getApplicationCores() / 2) + core_id); //core_id + 32
                                    reschedule(time, (Sim()->getConfig()->getApplicationCores() / 2) + core_id, true);
                              }
                        }
                  }

                  else //no apps on big cores to kick
                  {
                        //invalidate all threads for the highest kick priority app
                  }
            }

            else // move apps from little to big ones
            {
                  if (MyMigrationEnabled == true)
                  {
                        if (blacklist_candidate != -1)
                              MyPowerBlackList[blacklist_candidate] = 1;

                        moved_app_to_big = false;
                        for (int i = 0; i < MyAppsCount; i++)
                        {
                              if (MyIsBig[i] == 0 && MyPowerBlackList[i] == 0)
                              {
                                    printf("\nMoving App %d to Big cores\n", i);
                                    MyIsBig[i] = 1;

                                    if (to_be_kicked_app != -1)
                                          MyAppsKickPriority[i] = MyAppsKickPriority[to_be_kicked_app] + 1;
                                    else
                                          MyAppsKickPriority[i] = 0;

                                    for (thread_id_t thread_id = 0; thread_id < (thread_id_t)Sim()->getThreadManager()->getNumThreads(); ++thread_id)
                                    {
                                          app_id = Sim()->getThreadManager()->getThreadFromID(thread_id)->getAppId();

                                          if (app_id == i)
                                          {
                                                core_id_t core_id;
                                                for (core_id = 0; !(m_thread_info[thread_id].hasAffinity(core_id)); ++core_id)
                                                      ;

                                                m_thread_info[thread_id].setAffinitySingle(core_id - (Sim()->getConfig()->getApplicationCores() / 2));
                                                reschedule(time, core_id - (Sim()->getConfig()->getApplicationCores() / 2), true);
                                          }
                                    }
                                    moved_app_to_big = true;
                                    break; // we found the app that is needed to moved out from little to big ones
                              }
                        }
                  }
                  if (MyDvfsEnabled == true && moved_app_to_big == false) //if there is no candidate to move from little to big ones, try increasing frequency of the first app in the list
                  {
                        for (int i = 0; i < MyAppsCount; i++)
                        {
                              if (MyIsBig[i] == 1 && MyCurrentAppFreq[i] < MyMaxCoreFreq)
                              {
                                    if (MyCurrentAppFreq[i] + MyFreqDecStep < MyMaxCoreFreq)
                                          MyCurrentAppFreq[i] += MyFreqDecStep;
                                    else
                                          MyCurrentAppFreq[i] = MyMaxCoreFreq;

                                    for (thread_id_t thread_id = 0; thread_id < (thread_id_t)Sim()->getThreadManager()->getNumThreads(); ++thread_id)
                                    {
                                          app_id = Sim()->getThreadManager()->getThreadFromID(thread_id)->getAppId();

                                          if (app_id == i)
                                          {
                                                core_id_t core_id;
                                                for (core_id = 0; !(m_thread_info[thread_id].hasAffinity(core_id)); ++core_id)
                                                      ;

                                                Sim()->getMagicServer()->setFrequency(core_id, MyCurrentAppFreq[i]);
                                          }
                                    }

                                    break;
                              }
                        }
                  }
            }

            MyLastPower = MyPower;
      }
}

///////////////////////////...
void SchedulerPinnedBase::periodic(SubsecondTime time)
{
      SubsecondTime delta = time - m_last_periodic;

      for (core_id_t core_id = 0; core_id < (core_id_t)Sim()->getConfig()->getApplicationCores(); ++core_id)
      {
            if (delta > m_quantum_left[core_id] || m_core_thread_running[core_id] == INVALID_THREAD_ID)
            {
                  reschedule(time, core_id, true);
            }
            else
            {
                  m_quantum_left[core_id] -= delta;
            }
      }

      ////////////////////////

      MyThreadsStateManager();
      MyAppsDeparture();
      MyUpdatePower();
      MyPowerEvents(time);

      ////////////////////////

      m_last_periodic = time;
}

void SchedulerPinnedBase::reschedule(SubsecondTime time, core_id_t core_id, bool is_periodic)
{

      thread_id_t current_thread_id = m_core_thread_running[core_id];

      if (current_thread_id != INVALID_THREAD_ID && Sim()->getThreadManager()->getThreadState(current_thread_id) == Core::INITIALIZING)
      {
            // Thread on this core is starting up, don't reschedule it for now
            return;
      }

      thread_id_t new_thread_id = INVALID_THREAD_ID;
      SInt64 max_score = INT64_MIN;

      for (thread_id_t thread_id = 0; thread_id < (thread_id_t)m_threads_runnable.size(); ++thread_id)
      {
            if (m_thread_info[thread_id].hasAffinity(core_id) // Thread is allowed to run on this core
                && m_threads_runnable[thread_id] == true      // Thread is not stalled
                && (!m_thread_info[thread_id].isRunning()     // Thread is not already running somewhere else
                    || m_thread_info[thread_id].getCoreRunning() == core_id))
            {

                  SInt64 score;
                  if (m_thread_info[thread_id].isRunning())
                        // Thread is currently running: negative score depending on how long it's already running
                        score = SInt64(m_thread_info[thread_id].getLastScheduledIn().getPS()) - time.getPS();
                  else
                        // Thread is not currently running: positive score depending on how long we have been waiting
                        score = time.getPS() - SInt64(m_thread_info[thread_id].getLastScheduledOut().getPS());

                  // Find thread that was scheduled the longest time ago
                  if (score > max_score)
                  {
                        new_thread_id = thread_id;
                        max_score = score;
                  }
            }
      }

      if (current_thread_id != new_thread_id)
      {
            // If a thread was running on this core, and we'll schedule another one, unschedule the current one
            if (current_thread_id != INVALID_THREAD_ID)
            {
                  m_thread_info[current_thread_id].setCoreRunning(INVALID_CORE_ID);
                  // Update last scheduled out time, with a small extra penalty to make sure we don't
                  // reconsider this thread in the same periodic() call but for a next core
                  m_thread_info[current_thread_id].setLastScheduledOut(time + SubsecondTime::PS(core_id));
                  moveThread(current_thread_id, INVALID_CORE_ID, time);
            }

            // Set core as running this thread *before* we call moveThread(), otherwise the HOOK_THREAD_RESUME callback for this
            // thread might see an empty core, causing a recursive loop of reschedulings
            m_core_thread_running[core_id] = new_thread_id;

            // If we found a new thread to schedule, move it here
            if (new_thread_id != INVALID_THREAD_ID)
            {
                  // If thread was running somewhere else: let that core know
                  if (m_thread_info[new_thread_id].isRunning())
                        m_core_thread_running[m_thread_info[new_thread_id].getCoreRunning()] = INVALID_THREAD_ID;
                  // Move thread to this core
                  m_thread_info[new_thread_id].setCoreRunning(core_id);
                  m_thread_info[new_thread_id].setLastScheduledIn(time);
                  moveThread(new_thread_id, core_id, time);
            }
      }

      m_quantum_left[core_id] = m_quantum;
}

String SchedulerPinnedBase::ThreadInfo::getAffinityString() const
{
      std::stringstream ss;

      for (core_id_t core_id = 0; core_id < (core_id_t)Sim()->getConfig()->getApplicationCores(); ++core_id)
      {
            if (hasAffinity(core_id))
            {
                  if (ss.str().size() > 0)
                        ss << ",";
                  ss << core_id;
            }
      }
      return String(ss.str().c_str());
}

void SchedulerPinnedBase::printState()
{
      printf("thread state:");
      for (thread_id_t thread_id = 0; thread_id < (thread_id_t)Sim()->getThreadManager()->getNumThreads(); ++thread_id)
      {
            char state;
            switch (Sim()->getThreadManager()->getThreadState(thread_id))
            {
            case Core::INITIALIZING:
                  state = 'I';
                  break;
            case Core::RUNNING:
                  state = 'R';
                  break;
            case Core::STALLED:
                  state = 'S';
                  break;
            case Core::SLEEPING:
                  state = 's';
                  break;
            case Core::WAKING_UP:
                  state = 'W';
                  break;
            case Core::IDLE:
                  state = 'I';
                  break;
            case Core::BROKEN:
                  state = 'B';
                  break;
            case Core::NUM_STATES:
            default:
                  state = '?';
                  break;
            }
            if (m_thread_info[thread_id].isRunning())
            {
                  printf(" %c@%d", state, m_thread_info[thread_id].getCoreRunning());
            }
            else
            {
                  printf(" %c%c%s", state, m_threads_runnable[thread_id] ? '+' : '_', m_thread_info[thread_id].getAffinityString().c_str());
            }
      }
      printf("  --  core state:");
      for (core_id_t core_id = 0; core_id < (core_id_t)Sim()->getConfig()->getApplicationCores(); ++core_id)
      {
            if (m_core_thread_running[core_id] == INVALID_THREAD_ID)
                  printf(" __");
            else
                  printf(" %2d", m_core_thread_running[core_id]);
      }
      printf("\n");
}
