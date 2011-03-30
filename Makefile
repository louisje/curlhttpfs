URL="http://cds.sun.com/is-bin/INTERSHOP.enfinity/WFS/CDS-CDS_Developer-Site/en_US/-/USD/VerifyItem-Start/java_ee_sdk-6u2-unix-ml.sh?BundledLineItemUUID=DqiJ_hCy3.YAAAEv0iQIRwRz&OrderID=DU.J_hCyakoAAAEvwCQIRwRz&ProductID=ZdyJ_hCyVuAAAAEurb8Hek4c&FileName=/java_ee_sdk-6u2-unix-ml.sh"
FILENAME="java_ee_sdk-6u2-unix-ml.sh"

curlhttpfs: curlhttpfs.c
	gcc -Wall -I/usr/include/fuse/ -D_FILE_OFFSET_BITS=64 curlhttpfs.c -o curlhttpfs -lcurl -lpthread -lfuse

clean: umount
	trash curlhttpfs

mount: curlhttpfs umount
	./curlhttpfs -o url=${URL},filename=${FILENAME},allow_root remote/

umount:
	-fusermount -uz remote/
