# When changing this file, you need to bump the following
# .gitlab-ci/image-tags.yml tags:
# ALPINE_X86_64_LAVA_TRIGGER_TAG

from .console_format import CONSOLE_LOG
from .gitlab_section import GitlabSection
from .lava_job import LAVAJob
from .lava_job_definition import LAVAJobDefinition
from .lava_proxy import call_proxy, setup_lava_proxy
from .log_follower import (
    LogFollower,
    fatal_err,
    fix_lava_gitlab_section_log,
    hide_sensitive_data,
    print_log,
)
from .log_section import (
    DEFAULT_GITLAB_SECTION_TIMEOUTS,
    FALLBACK_GITLAB_SECTION_TIMEOUT,
    LogSection,
    LogSectionType,
    CI_JOB_TIMEOUT_MIN,
    LAVA_TEST_OVERHEAD_MIN,
    LAVA_TEST_CASE_TIMEOUT,
    LAVA_TEST_SUITE_TIMEOUT,
)
