all: cffs.1.html

cffs.1.html : ../cffs.1
	troff -man -Tascii ../cffs.1 |grotty -bc|man2html -sun -title 'Manual page for cffs' >cffs.1.html
