#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd -- "${script_dir}/.." && pwd)"
host_path="${project_dir}/extension/native/leafreader_native_host.py"
template_path="${project_dir}/extension/native/com.leafreader.piper.json.in"

if [[ ! -x "${host_path}" ]]; then
  chmod 755 "${host_path}"
fi

for config_root in "${XDG_CONFIG_HOME:-${HOME}/.config}/chromium" \
                   "${XDG_CONFIG_HOME:-${HOME}/.config}/google-chrome"; do
  target_dir="${config_root}/NativeMessagingHosts"
  mkdir -p "${target_dir}"
  sed "s|@HOST_PATH@|${host_path}|g" "${template_path}" \
    > "${target_dir}/com.leafreader.piper.json"
  printf 'Installed native host manifest: %s\n' "${target_dir}/com.leafreader.piper.json"
done
