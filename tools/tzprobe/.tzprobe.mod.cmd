savedcmd_tzprobe.mod := printf '%s\n'   tzprobe.o | awk '!x[$$0]++ { print("./"$$0) }' > tzprobe.mod
