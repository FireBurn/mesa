#!/bin/bash
# shellcheck disable=SC1090
# shellcheck disable=SC1091
# shellcheck disable=SC2086 # we want word splitting
# shellcheck disable=SC2155

# Second-stage init, used to set up devices and our job environment before
# running tests.

shopt -s extglob

# Make sure to kill itself and all the children process from this script on
# exiting, since any console output may interfere with LAVA signals handling,
# which based on the log console.
cleanup() {
  if [ "$BACKGROUND_PIDS" = "" ]; then
    return 0
  fi

  set +x
  echo "Killing all child processes"
  for pid in $BACKGROUND_PIDS
  do
    kill "$pid" 2>/dev/null || true
  done

  # Sleep just a little to give enough time for subprocesses to be gracefully
  # killed. Then apply a SIGKILL if necessary.
  sleep 5
  for pid in $BACKGROUND_PIDS
  do
    kill -9 "$pid" 2>/dev/null || true
  done

  BACKGROUND_PIDS=
  set -x
}
trap cleanup INT TERM EXIT

# Space separated values with the PIDS of the processes started in the
# background by this script
BACKGROUND_PIDS=


for path in '/dut-env-vars.sh' '/set-job-env-vars.sh' './set-job-env-vars.sh'; do
    [ -f "$path" ] && source "$path"
done
. "$SCRIPTS_DIR"/setup-test-env.sh

# Flush out anything which might be stuck in a serial buffer
echo
echo
echo

section_switch init_stage2 "Pre-testing hardware setup"

set -ex

# Set up any devices required by the jobs
[ -z "$HWCI_KERNEL_MODULES" ] || {
    echo -n $HWCI_KERNEL_MODULES | xargs -d, -n1 /usr/sbin/modprobe
}

# Set up ZRAM
HWCI_ZRAM_SIZE=2G
if /sbin/zramctl --find --size $HWCI_ZRAM_SIZE -a zstd; then
    mkswap /dev/zram0
    swapon /dev/zram0
    echo "zram: $HWCI_ZRAM_SIZE activated"
else
    echo "zram: skipping, not supported"
fi

#
# Load the KVM module specific to the detected CPU virtualization extensions:
# - vmx for Intel VT
# - svm for AMD-V
#
if [ -n "$HWCI_ENABLE_X86_KVM" ]; then
    unset KVM_KERNEL_MODULE
    {
      grep -qs '\bvmx\b' /proc/cpuinfo && KVM_KERNEL_MODULE=kvm_intel
    } || {
      grep -qs '\bsvm\b' /proc/cpuinfo && KVM_KERNEL_MODULE=kvm_amd
    }

    {
      [ -z "${KVM_KERNEL_MODULE}" ] && \
      echo "WARNING: Failed to detect CPU virtualization extensions"
    } || \
        modprobe ${KVM_KERNEL_MODULE}
fi

# Fix prefix confusion: the build installs to $CI_PROJECT_DIR, but we expect
# it in /install
ln -sf $CI_PROJECT_DIR/install /install
export LD_LIBRARY_PATH=/install/lib
export LIBGL_DRIVERS_PATH=/install/lib/dri

# https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/22495#note_1876691
# The navi21 boards seem to have trouble with ld.so.cache, so try explicitly
# telling it to look in /usr/local/lib.
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/usr/local/lib

# The Broadcom devices need /usr/local/bin unconditionally added to the path
export PATH=/usr/local/bin:$PATH

# Store Mesa's disk cache under /tmp, rather than sending it out over NFS.
export XDG_CACHE_HOME=/tmp

# Make sure Python can find all our imports
export PYTHONPATH=$(python3 -c "import sys;print(\":\".join(sys.path))")

# If we need to specify a driver, it means several drivers could pick up this gpu;
# ensure that the other driver can't accidentally be used
if [ -n "$MESA_LOADER_DRIVER_OVERRIDE" ]; then
  rm /install/lib/dri/!($MESA_LOADER_DRIVER_OVERRIDE)_dri.so
fi
ls -1 /install/lib/dri/*_dri.so || true

if [ "$HWCI_FREQ_MAX" = "true" ]; then
  # Ensure initialization of the DRM device (needed by MSM)
  head -0 /dev/dri/renderD128

  # Disable GPU frequency scaling
  DEVFREQ_GOVERNOR=$(find /sys/devices -name governor | grep gpu || true)
  test -z "$DEVFREQ_GOVERNOR" || echo performance > $DEVFREQ_GOVERNOR || true

  # Disable CPU frequency scaling
  echo performance | tee -a /sys/devices/system/cpu/cpufreq/policy*/scaling_governor || true

  # Disable GPU runtime power management
  GPU_AUTOSUSPEND=$(find /sys/devices -name autosuspend_delay_ms | grep gpu | head -1)
  test -z "$GPU_AUTOSUSPEND" || echo -1 > $GPU_AUTOSUSPEND || true
  # Lock Intel GPU frequency to 70% of the maximum allowed by hardware
  # and enable throttling detection & reporting.
  # Additionally, set the upper limit for CPU scaling frequency to 65% of the
  # maximum permitted, as an additional measure to mitigate thermal throttling.
  /install/common/intel-gpu-freq.sh -s 70% --cpu-set-max 65% -g all -d
fi

# Start a little daemon to capture sysfs records and produce a JSON file
KDL_PATH=/install/common/kdl.sh
if [ -x "$KDL_PATH" ]; then
  echo "launch kdl.sh!"
  $KDL_PATH &
  BACKGROUND_PIDS="$! $BACKGROUND_PIDS"
else
  echo "kdl.sh not found!"
fi

# Increase freedreno hangcheck timer because it's right at the edge of the
# spilling tests timing out (and some traces, too)
if [ -n "$FREEDRENO_HANGCHECK_MS" ]; then
    echo $FREEDRENO_HANGCHECK_MS | tee -a /sys/kernel/debug/dri/128/hangcheck_period_ms
fi

# Start a little daemon to capture the first devcoredump we encounter.  (They
# expire after 5 minutes, so we poll for them).
CAPTURE_DEVCOREDUMP=/install/common/capture-devcoredump.sh
if [ -x "$CAPTURE_DEVCOREDUMP" ]; then
  $CAPTURE_DEVCOREDUMP &
  BACKGROUND_PIDS="$! $BACKGROUND_PIDS"
fi

ARCH=$(uname -m)
export VK_DRIVER_FILES="/install/share/vulkan/icd.d/${VK_DRIVER}_icd.$ARCH.json"

# If we want Xorg to be running for the test, then we start it up before the
# HWCI_TEST_SCRIPT because we need to use xinit to start X (otherwise
# without using -displayfd you can race with Xorg's startup), but xinit will eat
# your client's return code
if [ -n "$HWCI_START_XORG" ]; then
  echo "touch /xorg-started; sleep 100000" > /xorg-script
  env \
    xinit /bin/sh /xorg-script -- /usr/bin/Xorg -noreset -s 0 -dpms -logfile "$RESULTS_DIR/Xorg.0.log" &
  BACKGROUND_PIDS="$! $BACKGROUND_PIDS"

  # Wait for xorg to be ready for connections.
  for _ in 1 2 3 4 5; do
    if [ -e /xorg-started ]; then
      break
    fi
    sleep 5
  done
  export DISPLAY=:0
fi

if [ -n "$HWCI_START_WESTON" ]; then
  WESTON_X11_SOCK="/tmp/.X11-unix/X0"
  if [ -n "$HWCI_START_XORG" ]; then
    echo "Please consider dropping HWCI_START_XORG and instead using Weston XWayland for testing."
    WESTON_X11_SOCK="/tmp/.X11-unix/X1"
  fi
  export WAYLAND_DISPLAY=wayland-0

  # Display server is Weston Xwayland when HWCI_START_XORG is not set or Xorg when it's
  export DISPLAY=:0
  mkdir -p /tmp/.X11-unix

  env weston --config="/install/common/weston.ini" -Swayland-0 --use-gl &
  BACKGROUND_PIDS="$! $BACKGROUND_PIDS"

  while [ ! -S "$WESTON_X11_SOCK" ]; do sleep 1; done
fi

set +x

section_end init_stage2

echo "Running ${HWCI_TEST_SCRIPT} ${HWCI_TEST_ARGS} ..."

set +e
$HWCI_TEST_SCRIPT ${HWCI_TEST_ARGS:-}; EXIT_CODE=$?
set -e

section_start post_test_cleanup "Cleaning up after testing, uploading results"
set -x

# Make sure that capture-devcoredump is done before we start trying to tar up
# artifacts -- if it's writing while tar is reading, tar will throw an error and
# kill the job.
cleanup

# upload artifacts (lava jobs)
if [ -n "$S3_RESULTS_UPLOAD" ]; then
  tar --zstd -cf results.tar.zst results/;
  ci-fairy s3cp --token-file "${S3_JWT_FILE}" results.tar.zst https://"$S3_RESULTS_UPLOAD"/results.tar.zst
fi

set +x
section_end post_test_cleanup

# Print the final result; both bare-metal and LAVA look for this string to get
# the result of our run, so try really hard to get it out rather than losing
# the run. The device gets shut down right at this point, and a630 seems to
# enjoy corrupting the last line of serial output before shutdown.
for _ in $(seq 0 3); do echo "hwci: mesa: exit_code: $EXIT_CODE"; sleep 1; echo; done

exit $EXIT_CODE
