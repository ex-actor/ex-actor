#include "ex_actor/internal/logging.h"

#include <exception>

#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>

namespace ex_actor::internal {

using testing::HasSubstr;
using testing::Not;

TEST(LoggingTest, NestableException) {
  try {
    try {
      throw logging::ExceptionWithStacktrace("level 0");
    } catch (std::exception& e) {
      std::string what = e.what();
      ASSERT_THAT(what, HasSubstr("ExceptionWithStacktrace, message: level 0"));
      ASSERT_THAT(what, HasSubstr("stack trace"));
      ASSERT_THAT(what, Not(HasSubstr("Cased by:")));
      throw logging::NestableException("level 1", std::current_exception());
    }
  } catch (std::exception& e) {
    std::string what = e.what();
    std::cout << what << std::endl;
    ASSERT_THAT(what, HasSubstr("NestableException, message: level 1"));
    ASSERT_THAT(what, HasSubstr("stack trace"));
    ASSERT_THAT(what, HasSubstr("Cased by:"));
    ASSERT_THAT(what, HasSubstr("ExceptionWithStacktrace, message: level 0"));
  }
}

}  // namespace ex_actor::internal