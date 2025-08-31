#pragma once

#include <exec/static_thread_pool.hpp>

namespace ex_actor {

using WorkStealingThreadPool = exec::static_thread_pool;

}  // namespace ex_actor