// Copyright 2010 Google
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef BASE_LOGGING_H
#define BASE_LOGGING_H

#include <iosfwd>

#include "mongo/util/log.h"

#include "macros.h"

// Always-on checking
#define CHECK(x)	if(x){}else LogMessageFatal(__FILE__, __LINE__).stream() << "Check failed: " #x
#define CHECK_LT(x, y)	CHECK((x) < (y))
#define CHECK_GT(x, y)	CHECK((x) > (y))
#define CHECK_LE(x, y)	CHECK((x) <= (y))
#define CHECK_GE(x, y)	CHECK((x) >= (y))
#define CHECK_EQ(x, y)	CHECK((x) == (y))
#define CHECK_NE(x, y)	CHECK((x) != (y))
#define CHECK_NOTNULL(x) CHECK((x) != NULL)

#ifndef NDEBUG
// Debug-only checking.
#define DCHECK(condition) CHECK(condition)
#define DCHECK_EQ(val1, val2) CHECK_EQ(val1, val2)
#define DCHECK_NE(val1, val2) CHECK_NE(val1, val2)
#define DCHECK_LE(val1, val2) CHECK_LE(val1, val2)
#define DCHECK_LT(val1, val2) CHECK_LT(val1, val2)
#define DCHECK_GE(val1, val2) CHECK_GE(val1, val2)
#define DCHECK_GT(val1, val2) CHECK_GT(val1, val2)
#else
#define DCHECK(condition) CHECK(false)
#define DCHECK_EQ(val1, val2) CHECK(false)
#define DCHECK_NE(val1, val2) CHECK(false)
#define DCHECK_LE(val1, val2) CHECK(false)
#define DCHECK_LT(val1, val2) CHECK(false)
#define DCHECK_GE(val1, val2) CHECK(false)
#define DCHECK_GT(val1, val2) CHECK(false)
#endif

#include "base/port.h"
#define INFO mongo::log().stream()
#define FATAL LogMessageFatal(__FILE__, __LINE__).stream()
#define DFATAL LogMessageFatal(__FILE__, __LINE__).stream()

#define S2LOG(x) x
#define VLOG(x) if (x>0) {} else S2LOG(INFO)

class LogMessageFatal {
 public:
  LogMessageFatal(const char* file, int line);
  ~LogMessageFatal();
  std::ostream& stream() { return _lsb.stream(); }

 private:
    mongo::logger::LogstreamBuilder _lsb;
  DISALLOW_COPY_AND_ASSIGN(LogMessageFatal);
};

#endif  // BASE_LOGGING_H
