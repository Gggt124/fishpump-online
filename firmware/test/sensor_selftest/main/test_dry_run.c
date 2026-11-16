#include <string.h>
#include "selftest.h"
#include "dry_run_logic.h"

void test_dry_run_config(void)
{
    dry_run_config_t c;
    dry_run_config_defaults(&c);
    CHECK(!c.enabled && c.prime_sec == 15 && c.dry_trip_sec == 10 && c.retry_wait_min == 5 &&
          c.retry_backoff && c.max_retries == 3);
    const char *bad = "";
    CHECK(dry_run_config_validate(&c, &bad));
    c.prime_sec = 0;   CHECK(!dry_run_config_validate(&c, &bad) && strcmp(bad, "prime_sec") == 0);
    c.prime_sec = 601; CHECK(!dry_run_config_validate(&c, &bad) && strcmp(bad, "prime_sec") == 0);
    dry_run_config_defaults(&c); c.dry_trip_sec = 2;   CHECK(!dry_run_config_validate(&c, &bad) && strcmp(bad, "dry_trip_sec") == 0);
    dry_run_config_defaults(&c); c.retry_wait_min = 0; CHECK(!dry_run_config_validate(&c, &bad) && strcmp(bad, "retry_wait_min") == 0);
    dry_run_config_defaults(&c); c.max_retries = 6;    CHECK(!dry_run_config_validate(&c, &bad) && strcmp(bad, "max_retries") == 0);
    dry_run_config_defaults(&c);
    CHECK(dry_run_wait_ms(&c, 1) == 5u * 60000u);
    CHECK(dry_run_wait_ms(&c, 2) == 10u * 60000u);
    CHECK(dry_run_wait_ms(&c, 3) == 20u * 60000u);
    c.retry_backoff = false; CHECK(dry_run_wait_ms(&c, 3) == 5u * 60000u);
    c.retry_backoff = true; c.retry_wait_min = 240; CHECK(dry_run_wait_ms(&c, 5) == 240u * 60000u); /* cap */
    CHECK(dry_run_suggest_prime_sec(8000) == 17);      /* ceil(12) + 5 */
    CHECK(dry_run_suggest_prime_sec(8100) == 18);      /* ceil(12.15) + 5 */
    CHECK(dry_run_suggest_prime_sec(0) == 5);
    CHECK(dry_run_suggest_prime_sec(1000000) == 600);  /* clamp */
}
