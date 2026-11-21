# Create firmware/components/app_config/secrets.h with a new random OTA key.
# Prints only the path and the key fingerprint. Copy the key from the file into
# a password manager and the GitHub secret OTA_ENCRYPTION_KEY (docs/release/keys.md).
python (Join-Path $PSScriptRoot "write_secrets.py") --generate @args
exit $LASTEXITCODE
