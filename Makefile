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
#URL="http://ftp.uni-kl.de/pub/linux/knoppix-dvd/KNOPPIX_V6.2DVD-2009-11-18-EN.iso"
#URL="http://ftp.cs.pu.edu.tw/Linux/Knoppix/KNOPPIX_V6.2CD-2009-11-18-EN.iso"
#URL="http://ftp.cs.pu.edu.tw/Linux/CentOS/5.4/isos/i386/CentOS-5.4-i386-bin-DVD.iso"
#URL="http://ftp.cs.pu.edu.tw/Linux/CentOS/5.4/isos/i386/CentOS-5.4-i386-LiveCD.iso"
#URL="http://ftp.belnet.be/mirror/pclinuxonline.com/live-cd/english/preview/pclinuxos-minime-kde3-2009.1.iso"
#URL="http://ftp.isu.edu.tw/pub/Linux/Mandriva/devel/iso/2010.0/rc2/mandriva-linux-free-dual-rc2-2010.iso"
#URL="http://ftp.cs.pu.edu.tw/pub/opensolaris/2009/06/osol-0906-x86.iso"
#URL="http://ftp.cs.pu.edu.tw/Linux/OpenSuse/distribution/11.2-RC2/iso/openSUSE-GNOME-LiveCD-Build0339-i686.iso"
#URL="http://dl8-cdn-02.sun.com/s/ESD6/JSCDL/jdk/6u17-b04/jre-6u17-linux-i586.bin?e=1258628508565&h=c3c6d810bd52a9e39cf5e61ba610e160/&filename=jre-6u17-linux-i586.bin"
#URL="http://ftp.cs.pu.edu.tw/Linux/Debian/debian-cd/5.0.2-live/i386/usb-hdd/debian-live-502-i386-gnome-desktop.img"
#URL="http://nimue.fit.vutbr.cz/slax/SLAX-6.x/slax-6.1.2.iso"
#URL="http://www.cpuid.com/download/cpuz/cpuz_152.zip"
#URL="http://nchc.dl.sourceforge.net/project/portableapps/OpenOffice.org%20Portable/OpenOffice.org%20Portable%203.1.1/OpenOfficePortable_3.1.1_English.paf.exe"
URL="http://media-2.cacetech.com/wireshark/win32/wireshark-win32-1.2.4.exe"
FILENAME=""

curlhttpfs: curlhttpfs.c
	gcc -Wall -I/usr/include/fuse/ -D_FILE_OFFSET_BITS=64 curlhttpfs.c -o curlhttpfs -lcurl -lfuse -lpthread

clean: unmount
	trash curlhttpfs

mount: curlhttpfs unmount
	./curlhttpfs -o url=${URL},filename="${FILENAME}",allow_root remote/

unmount:
	-fusermount -uz remote/
