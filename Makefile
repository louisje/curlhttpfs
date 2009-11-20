URL="http://file.buda.idv.tw/multi/djs.rm"
FILENAME=""

curlhttpfs: curlhttpfs.c
	gcc -static -Wall -I/usr/include/fuse/ -D_FILE_OFFSET_BITS=64 curlhttpfs.c -o curlhttpfs -lcurl -lfuse -lpthread

clean: unmount
	trash curlhttpfs

mount: curlhttpfs unmount
	./curlhttpfs -o url="${URL}",filename="${FILENAME}",allow_root remote/

unmount:
	-fusermount -uz remote/
