# mmap-tail-f
tail follow memory mapped files which tail cannot follow
Certain high performance logging applications use a memory mapped file which is pre-filled with NULL.
log messages are written with in this char *buf, at subsequent indices.

an example file would look like:
# code
char buf[4096];
snprintf(buf, 4096, "some text here\n");

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
