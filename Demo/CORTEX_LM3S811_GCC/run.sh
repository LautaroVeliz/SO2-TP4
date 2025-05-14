make clean

make

echo "--- Launching QEMU ---"
qemu-system-arm -machine lm3s811evb -kernel gcc/RTOSDemo.axf -serial stdio 
#qemu-system-arm -machine lm3s811evb -kernel gcc/RTOSDemo.axf -serial stdio -s -S
#qemu-system-arm -machine lm3s811evb -kernel gcc/RTOSDemo.axf -serial pty
