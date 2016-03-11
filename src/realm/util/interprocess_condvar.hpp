/*************************************************************************
 *
 * REALM CONFIDENTIAL
 * __________________
 *
 *  [2011] - [2015] Realm Inc
 *  All Rights Reserved.
 *
 * NOTICE:  All information contained herein is, and remains
 * the property of Realm Incorporated and its suppliers,
 * if any.  The intellectual and technical concepts contained
 * herein are proprietary to Realm Incorporated
 * and its suppliers and may be covered by U.S. and Foreign Patents,
 * patents in process, and are protected by trade secret or copyright law.
 * Dissemination of this information or reproduction of this material
 * is strictly forbidden unless prior written permission is obtained
 * from Realm Incorporated.
 *
 **************************************************************************/

#ifndef REALM_UTIL_INTERPROCESS_CONDVAR
#define REALM_UTIL_INTERPROCESS_CONDVAR


#include <realm/util/features.h>
#include <realm/util/thread.hpp>
#include <realm/util/interprocess_mutex.hpp>
#include <stdint.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <mutex>

// Condvar Emulation is required if RobustMutex emulation is enabled
#ifdef REALM_ROBUST_MUTEX_EMULATION
#define REALM_CONDVAR_EMULATION
#endif

namespace realm {
namespace util {




/// Condition variable for use in synchronization monitors.
/// This condition variable uses emulation based on named pipes
/// for the inter-process case, if enabled by REALM_CONDVAR_EMULATION.
///
/// FIXME: This implementation will never release/delete pipes. This is unlikely
/// to be a problem as long as only a modest number of different database names
/// are in use
///
/// A InterprocessCondVar is always process shared.
class InterprocessCondVar {
public:
    InterprocessCondVar();
    ~InterprocessCondVar() noexcept;

    /// To use the InterprocessCondVar, you also must place a structure of type
    /// InterprocessCondVar::SharedPart in memory shared by multiple processes
    /// or in a memory mapped file, and use set_shared_part() to associate
    /// the condition variable with it's shared part. You must initialize
    /// the shared part using InterprocessCondVar::init_shared_part(), but only before
    /// first use and only when you have exclusive access to the shared part.

#ifdef REALM_CONDVAR_EMULATION
    struct SharedPart {
        uint64_t signal_counter;
        uint64_t wait_counter;
    };
#else
    typedef CondVar SharedPart;
#endif

    /// You need to bind the emulation to a SharedPart in shared/mmapped memory.
    /// The SharedPart is assumed to have been initialized (possibly by another process)
    /// earlier through a call to init_shared_part.
    void set_shared_part(SharedPart& shared_part, std::string path, std::string condvar_name);

    /// Initialize the shared part of a process shared condition variable.
    /// A process shared condition variables may be represented by any number of
    /// InterprocessCondVar instances in any number of different processes,
    /// all sharing a common SharedPart instance, which must be in shared memory.
    static void init_shared_part(SharedPart& shared_part);

    /// Wait for someone to call notify() or notify_all() on this condition
    /// variable. The call to wait() may return spuriously, so the caller should
    /// always re-evaluate the condition on which to wait and loop on wait()
    /// if necessary.
    void wait(InterprocessMutex& m, const struct timespec* tp);

    /// If any threads are waiting for this condition, wake up at least one.
    /// (Current implementation may actually wake all :-O ). The caller must
    /// hold the lock associated with the condvar at the time of calling notify()
    void notify() noexcept;

    /// Wake up every thread that is currently waiting on this condition.
    /// The caller must hold the lock associated with the condvar at the time
    /// of calling notify_all().
    void notify_all() noexcept;

    /// Cleanup and release system resources if possible.
    void close() noexcept;

private:
    // non-zero if a shared part has been registered (always 0 on process local instances)
    SharedPart* m_shared_part = nullptr;

    bool uses_emulation = false;
    // pipe used for emulation
    int m_fd_read = -1;
    int m_fd_write = -1;
};




// Implementation:


} // namespace util
} // namespace realm


#endif
