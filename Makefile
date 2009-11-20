URL="http://public.blu.livefilestore.com/y1pVFcWMbvio7erfgTfUctlgJNNLIHAKhdf6YiuZysgPzGW7ix3XABmJSh3aAfWdoWL8SX2fpMvCtaUmCRdwxGNmQ/spelunky_0_99_5.rar?download"
FILENAME=""

curlhttpfs: curlhttpfs.c
	gcc -Wall -I/usr/include/fuse/ -D_FILE_OFFSET_BITS=64 curlhttpfs.c -o curlhttpfs -lcurl -lpthread -lfuse

clean: unmount
	trash curlhttpfs

mount: curlhttpfs unmount
	./curlhttpfs -o url=${URL},filename=${FILENAME},allow_root remote/

unmount:
	-fusermount -uz remote/
