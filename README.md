# mmap-tail-f
Many times, when debugging/monitoring, it is useful to follow additions at the end of a while.
For instance, a log file to which lines are added everytime something happens.
the linux tail utility allows you to see the last n lines of a file.
with tail -f, you can make tail "follow" the output and not exit when it reaches the end of while.
Instead, it waits and checks if newer output has appeared, periodically, and prints as it appears.

This utility allows you to follow memory mapped files, which the standard tail utility cannot follow.
Certain high performance logging applications use a memory mapped file which is pre-filled with NULL.
log messages are written with in this char *buf, at subsequent indices.

an example file would look like:
# code
char buf[4096];
snprintf(buf, 4096, "some text here\n");
/* after some time */
snprintf(buf+pos, 4096-pos, "new text at a later time\n"

# log file
Now the file would look like

hexdump -C test_file | head -n 2

00000000  73 6f 6d 65 20 74 65 78  74 20 68 65 72 65 0a 00  |some text here..|

00000010  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|


Following with normal bash-utilities is difficult, as the usual parameters which change for ordinary files do not change here.
inotify-wait also doesn't work for this type of file.

a simple idea would be to write a bash script which can repeatedly poll the number of lines in the file. 
If that changes, use awk/sed to print the newer lines.

# bash pseudocode:
while true; do
    old = $(wc -l $filename)
    sleep $poll_interval
    new = $(wc -l $filename)
    sed -n "${$old},${new}p;${new}q" $(filename)
    old = $new
done

However, this can be very inefficient for larger files. 
Also, to handle a lot of files, it might be even more cumbersome.

mtail-f provides a tail like utility with similar functions/options to follow such files

# usage:
mtail-f <filename>
mtail-f <filename1> <filename2> ...

Use ctrl-c to exit

# Building
simply use gcc to build the stand-alone mtail-f.c file and copy the binary to a location in your PATH
