#!/bin/bash -ex

export NDK_CCACHE=$(which ccache)

if [ ! -z "${ANDROID_KEYSTORE_B64}" ]; then
    export ANDROID_KEYSTORE_FILE="${GITHUB_WORKSPACE}/ks.jks"
    base64 --decode <<< "${ANDROID_KEYSTORE_B64}" > "${ANDROID_KEYSTORE_FILE}"
fi

cd src/android
chmod +x ./gradlew

# assembleRelease packages all product flavors (googlePlay/vanilla/thor)
# in one invocation and has repeatedly hit AGP's own
# PackageAndroidArtifact$IncrementalSplitterRunnable flake under CI --
# a transient failure with no useful stack trace and no code change
# that fixes it (root cause is inside AGP's own incremental packaging,
# not this project), so retrying the whole command is the standard
# mitigation rather than re-running the CI job by hand each time.
gradle_retry() {
    local attempt
    for attempt in 1 2 3; do
        if ./gradlew "$@"; then
            return 0
        fi
        echo "gradle $* failed (attempt $attempt/3), retrying..." >&2
    done
    return 1
}

gradle_retry assembleRelease
gradle_retry bundleRelease

ccache -s -v

if [ ! -z "${ANDROID_KEYSTORE_B64}" ]; then
    rm "${ANDROID_KEYSTORE_FILE}"
fi
