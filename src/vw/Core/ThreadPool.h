// __BEGIN_LICENSE__
// 
// Copyright (C) 2006 United States Government as represented by the
// Administrator of the National Aeronautics and Space Administration
// (NASA).  All Rights Reserved.
// 
// Copyright 2006 Carnegie Mellon University. All rights reserved.
// 
// This software is distributed under the NASA Open Source Agreement
// (NOSA), version 1.3.  The NOSA has been approved by the Open Source
// Initiative.  See the file COPYING at the top of the distribution
// directory tree for the complete NOSA document.
// 
// THE SUBJECT SOFTWARE IS PROVIDED "AS IS" WITHOUT ANY WARRANTY OF ANY
// KIND, EITHER EXPRESSED, IMPLIED, OR STATUTORY, INCLUDING, BUT NOT
// LIMITED TO, ANY WARRANTY THAT THE SUBJECT SOFTWARE WILL CONFORM TO
// SPECIFICATIONS, ANY IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR
// A PARTICULAR PURPOSE, OR FREEDOM FROM INFRINGEMENT, ANY WARRANTY THAT
// THE SUBJECT SOFTWARE WILL BE ERROR FREE, OR ANY WARRANTY THAT
// DOCUMENTATION, IF PROVIDED, WILL CONFORM TO THE SUBJECT SOFTWARE.
// 
// __END_LICENSE__

/// \file Core/ThreadsPool.h
/// 
/// Note: All tasks need to be of the same type, but you can have a
/// common abstract base class if you want.
///
#ifndef __VW_CORE_THREADPOOL_H__
#define __VW_CORE_THREADPOOL_H__

#include <vw/Core/Thread.h>
#include <vw/Core/Debugging.h>
#include <vw/Core/Exception.h>

// STL
#include <map>

namespace vw {
  // ----------------------  --------------  ---------------------------
  // ----------------------       Task       ---------------------------
  // ----------------------  --------------  ---------------------------
  struct Task {
    virtual ~Task() {}
    virtual void operator()() = 0;
  };

  // ----------------------  --------------  ---------------------------
  // ----------------------  Task Generator  ---------------------------
  // ----------------------  --------------  ---------------------------

  // Work Queue Base Class
  class WorkQueue {

    // The worker thread class is the Task object that is spun out to
    // do the actual work of the WorkQueue.  When a worker thread
    // finishes its task it notifies the threadpool, which farms out
    // the next task to the worker thread.
    class WorkerThread {
      WorkQueue &m_queue;
      boost::shared_ptr<Task> m_task;
      int m_thread_id;
    public:
      WorkerThread(WorkQueue& queue, boost::shared_ptr<Task> initial_task, int thread_id) :
        m_queue(queue), m_task(initial_task), m_thread_id(thread_id) {}
      ~WorkerThread() {}
      void operator()() { 
        // Run the initial task
        (*m_task)();

        // ... and continue running tasks as long as they are available.
        while ( m_task = m_queue.get_next_task() ) {
          vw_out(DebugMessage, "thread") << "ThreadPool: reusing worker thread.";
          (*m_task)();
        }

        m_queue.worker_thread_complete(m_thread_id); 
      }
    };

    int m_active_workers, m_max_workers;
    Mutex m_queue_mutex;
    std::vector<boost::shared_ptr<Thread> > m_running_threads;
    std::list<int> m_available_thread_ids;
    Condition m_joined_event;

    // This is called whenever a worker thread finishes its task. If
    // there are more tasks available, the worker is given more work.
    // Otherwise, the worker terminates.
    void worker_thread_complete(int worker_id) {
      Mutex::Lock lock(m_queue_mutex);
      
      m_active_workers--;
      vw_out(DebugMessage, "thread") << "ThreadPool: terminating worker thread " << worker_id << ".  [ " << m_active_workers << " / " << m_max_workers << " now active ]\n"; 
      
      // Erase the worker thread from the list of active threads
      VW_ASSERT(worker_id >= 0 && worker_id < int(m_running_threads.size()), 
                LogicErr() << "WorkQueue: request to terminate thread " << worker_id << ", which does not exist.");
      m_running_threads[worker_id] = boost::shared_ptr<Thread>();
      m_available_thread_ids.push_back(worker_id);

      // Notify any threads that are waiting for the join event.
      m_joined_event.notify_all();
    }

  public:
    WorkQueue(int num_threads = Thread::default_num_threads() ) : m_active_workers(0), m_max_workers(num_threads) {
      m_running_threads.resize(num_threads);
      for (int i = 0; i < num_threads; ++i)
        m_available_thread_ids.push_back(i);
    }
    virtual ~WorkQueue() { this->join_all(); }

    /// Return a shared pointer to the next task.  If no tasks are
    /// available, return an empty shared pointer.
    virtual boost::shared_ptr<Task> get_next_task() = 0;

    void notify() {
      Mutex::Lock lock(m_queue_mutex);

      // While there are available threads, farm out the tasks from
      // the task generator
      boost::shared_ptr<Task> task;
      while (m_available_thread_ids.size() != 0 && (task = this->get_next_task()) ) {
        int next_available_thread_id = m_available_thread_ids.front();
        m_available_thread_ids.pop_front();

        boost::shared_ptr<WorkerThread> next_worker( new WorkerThread(*this, task, next_available_thread_id) );
        boost::shared_ptr<Thread> thread(new Thread(next_worker));
        m_running_threads[next_available_thread_id] = thread;
        m_active_workers++;
        vw_out(DebugMessage, "thread") << "ThreadPool: creating worker thread " << next_available_thread_id << ".  [ " << m_active_workers << " / " << m_max_workers << " now active ]\n"; 
      } 
    }

    /// Return the max number threads that can run concurrently at any
    /// given time using this threadpool.
    int max_threads() { 
      Mutex::Lock lock(m_queue_mutex);
      return m_max_workers;
    }

    /// Return the max number threads that can run concurrently at any
    /// given time using this threadpool.
    int active_threads() { 
      Mutex::Lock lock(m_queue_mutex);
      return m_active_workers;
    }

    // Join all currently running threads and wait for the task pool to be empty.
    void join_all() {
      bool finished = false;
      
      // Wait for the threads to clean up the threadpool state and exit.
      while(!finished) {
        Mutex::Lock lock(m_queue_mutex);
        if (m_active_workers != 0) {
          m_joined_event.wait(lock);
        } else {
          finished = true;
        }
      }
    }

  };



  /// A simple, first-in, first-out work queue.
  class FifoWorkQueue : public WorkQueue {
    std::list<boost::shared_ptr<Task> > m_queued_tasks;
    Mutex m_mutex;
  public:

    FifoWorkQueue(int num_threads = Thread::default_num_threads()) : WorkQueue(num_threads) {}

    int size() { 
      Mutex::Lock lock(m_mutex);
      return m_queued_tasks.size(); 
    }
    
    // Add a task that is being tracked by a shared pointer.
    void add_task(boost::shared_ptr<Task> task) { 
      {
        Mutex::Lock lock(m_mutex);
        m_queued_tasks.push_back(task); 
      }
      this->notify();
    }

    virtual boost::shared_ptr<Task> get_next_task() { 
      Mutex::Lock lock(m_mutex);
      if (m_queued_tasks.size() == 0) 
        return boost::shared_ptr<Task>();

      boost::shared_ptr<Task> task = m_queued_tasks.front();
      m_queued_tasks.pop_front();
      return task;
    }
  };

  /// A simple ordered work queue.  Tasks are each given an "index"
  /// and they are processed in order starting with the task at index
  /// 0.  The idle() method returns true unless the task with the next
  /// expected index is present in the work queue.
  class OrderedWorkQueue : public WorkQueue {
    std::map<int, boost::shared_ptr<Task> > m_queued_tasks;
    int m_next_index;
    Mutex m_mutex;
  public:

    OrderedWorkQueue(int num_threads = Thread::default_num_threads()) : WorkQueue(num_threads) {
      m_next_index = 0;
    }

    int size() { 
      Mutex::Lock lock(m_mutex);
      return m_queued_tasks.size(); 
    }
    
    // Add a task that is being tracked by a shared pointer.
    void add_task(boost::shared_ptr<Task> task, int index) { 
      {
        Mutex::Lock lock(m_mutex);
        m_queued_tasks[index] = task;
      }
      notify();
    }

    virtual boost::shared_ptr<Task> get_next_task() { 

      // If there are no tasks available, we return the NULL task.
      if (m_queued_tasks.size() == 0) 
        return boost::shared_ptr<Task>();

      {
        Mutex::Lock lock(m_mutex);
        std::map<int, boost::shared_ptr<Task> >::iterator iter = m_queued_tasks.begin();

        // If the next task does not have the expected index, we
        // return the NULL task.
        if ((*iter).first != m_next_index) 
          return boost::shared_ptr<Task>();

       
        boost::shared_ptr<Task> task = (*iter).second;
        m_queued_tasks.erase(m_queued_tasks.begin());
        m_next_index++;
        return task;
      }
    }
  };

} // namespace vw

#endif // __VW_CORE_THREADPOOL_H__
