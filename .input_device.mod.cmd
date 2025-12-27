savedcmd_input_device.mod := printf '%s\n'   input_device.o | awk '!x[$$0]++ { print("./"$$0) }' > input_device.mod
