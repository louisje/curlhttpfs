#URL="http://202.5.224.193/louis/idownloader.wsdl"
#URL="http://202.5.224.193/louis/QQQx1000000.swf"
#URL="http://202.5.224.193/louis/autotools.pdf"
#URL="http://202.5.224.193/louis/video/D-life_sign_up.avi"
#URL="http://202.5.224.193/louis/diary.mp3"
#URL="http://ftp.stu.edu.tw/Linux/CentOS/5.4/isos/i386/CentOS-5.4-i386-LiveCD.iso"
#URL="http://ftp.cs.pu.edu.tw/Linux/Ubuntu/ubuntu-cd/karmic/ubuntu-9.10-desktop-i386.iso"
#URL="http://gentoo.osuosl.org//releases/x86/10.1/livedvd-x86-amd64-32ul-10.1.iso"
#URL="http://download.opensuse.org/distribution/SL-10.1/non-oss-dvd-iso/SUSE-Linux-10.1-GM-LiveDVD.iso"
#URL="http://192.168.0.89/~louis/GONGGONG.ISO"
#URL="http://ftp.heanet.ie/pub/linuxmint.com/stable/7/LinuxMint-7.iso"
#URL="http://mirror.cs.vt.edu/pub/MEPIS/SimplyMEPIS-CD_8.0.12-rel_32.iso"
URL="http://ftp.uni-kl.de/pub/linux/knoppix-dvd/KNOPPIX_V6.2DVD-2009-11-18-EN.iso"
#URL="http://ftp.cs.pu.edu.tw/Linux/Knoppix/KNOPPIX_V6.2CD-2009-11-18-EN.iso"
#URL="http://ftp.cs.pu.edu.tw/Linux/CentOS/5.4/isos/i386/CentOS-5.4-i386-bin-DVD.iso"
#URL="http://ftp.cs.pu.edu.tw/Linux/CentOS/5.4/isos/i386/CentOS-5.4-i386-LiveCD.iso"
#URL="http://ftp.belnet.be/mirror/pclinuxonline.com/live-cd/english/preview/pclinuxos-minime-kde3-2009.1.iso"
#URL="http://ftp.isu.edu.tw/pub/Linux/Mandriva/devel/iso/2010.0/rc2/mandriva-linux-free-dual-rc2-2010.iso"
#URL="http://ftp.cs.pu.edu.tw/pub/opensolaris/2009/06/osol-0906-x86.iso"
#URL="http://ftp.cs.pu.edu.tw/Linux/OpenSuse/distribution/11.2-RC2/iso/openSUSE-GNOME-LiveCD-Build0339-i686.iso"

curlhttpfs: curlhttpfs.c
	gcc -Wall -I/usr/include/fuse/ -D_FILE_OFFSET_BITS=64 curlhttpfs.c -o curlhttpfs -lcurl -lfuse -lpthread

clean: unmount
	trash curlhttpfs

mount: curlhttpfs unmount
	./curlhttpfs -o url=${URL},allow_root remote/

unmount:
	-fusermount -uz remote/
