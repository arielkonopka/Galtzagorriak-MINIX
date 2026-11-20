# /etc/homev3.b - the boot steps of setup -H (see /etc/homev3).
PATH=/bin:/usr/bin
st=/etc/.home-v3
step=`cat $st`
test $step = copy && echo "setup -H: copying /home to the swap partition /dev/hd3"
test $step = copy && {
	cd /home && find . -type f -exec cksum {} \; | sort >$st.sum
	set `tar cf - . | tee /dev/hd3 | cksum`
	echo "$1 $2" >$st.tar
	cd /
}
test $step = copy && {
	set `cat $st.tar`
	n=`expr $2 / 512`
	set `dd if=/dev/hd3 bs=512 count=$n 2>/dev/null | cksum` `cat $st.tar`
	test "$1" = "$3" -a "$2" = "$4" -a "$2" -gt 1024 || {
		echo "setup -H: the copy on /dev/hd3 does not read back the same:"
		echo "  nothing was changed, /home stays MINIX V2"
		rm -f $st $st.sum $st.tar
		exit 1
	}
	echo make >$st
	sync
	step=make
}
test $step = make && echo "setup -H: making a MINIX V3 /home on /dev/hd4 and copying it back"
test $step = make && {
	cd /
	umount /dev/hd4 >/dev/null 2>&1
	set `mkpart -l /dev/hd0 | grep '^4 '`
	h=`expr $5 / 2`
	mb=`mkpart -s /dev/hd0`
	g=`expr $mb "*" 1024 - $h`
	mkfs -3 -g $g /dev/hd4 && mount /dev/hd4 /home &&
	(cd /home && tar xopf /dev/hd3) || {
		echo "setup -H: this step failed; it runs again at the next boot."
		echo "  Your files are on /dev/hd3 (tar tvf /dev/hd3)."
		exit 1
	}
	echo check >$st
	sync
	step=check
}
test $step = check && echo "setup -H: checking every file"
test $step = check && {
	cd /home && find . -type f -exec cksum {} \; | sort >/tmp/home-v3.sum
	cd /
	cmp -s /tmp/home-v3.sum $st.sum || {
		echo failed >$st
		sync
		step=failed
	}
}
test $step = check && {
	rm -f $st $st.sum $st.tar /tmp/home-v3.sum
	sync
	echo "setup -H: /home is MINIX V3 now; every file is the same."
	exit 0
}
echo "setup -H: some files differ after the copy (/tmp/home-v3.sum, $st.sum)."
echo "  The copy stays on /dev/hd3 and swap stays off: cd /home; tar xopf /dev/hd3"
exit 1
