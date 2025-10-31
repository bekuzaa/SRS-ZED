//
// Copyright (c) 2013-2025 The SRS Authors
//
// SPDX-License-Identifier: MIT
//

#ifndef SRS_CORE_TIME_HPP
#define SRS_CORE_TIME_HPP

// Time and duration unit, in us.
#include <stdint.h>
typedef int64_t srs_utime_t;

// The time unit in ms, for example 100 * SRS_UTIME_MILLISECONDS means 100ms.
#define SRS_UTIME_MILLISECONDS 1000

// Convert srs_utime_t as ms.
#define srsu2ms(us) ((us) / SRS_UTIME_MILLISECONDS)
#define srsu2msi(us) int((us) / SRS_UTIME_MILLISECONDS)

// Convert srs_utime_t as second.
#define srsu2s(us) ((us) / SRS_UTIME_SECONDS)
#define srsu2si(us) int((us) / SRS_UTIME_SECONDS)

// Them time duration = end - start. return 0, if start or end is 0.
srs_utime_t srs_time_since(srs_utime_t start, srs_utime_t end);

// The time unit in seconds, for example 120 * SRS_UTIME_SECONDS means 120s.
#define SRS_UTIME_SECONDS 1000000LL

// The time unit in minutes, for example 3 * SRS_UTIME_MINUTES means 3m.
#define SRS_UTIME_MINUTES 60000000LL

// The time unit in hours, for example 2 * SRS_UTIME_HOURS means 2h.
#define SRS_UTIME_HOURS 3600000000LL

// Never timeout.
#define SRS_UTIME_NO_TIMEOUT ((srs_utime_t) - 1LL)

// Get current system time in srs_utime_t, use cache to avoid performance problem
extern srs_utime_t srs_time_now_cached();
extern srs_utime_t srs_time_since_startup();
// A daemon st-thread updates it.
extern srs_utime_t srs_time_now_realtime();

// The time interface.
class ISrsTime
{
public:
    ISrsTime();
    virtual ~ISrsTime();

public:
    virtual void usleep(srs_utime_t duration) = 0;
};

#endif
