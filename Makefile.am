SUBDIRS = src share
TESTS_ENVIRONMENT = $(SHELL) -x
TESTS = ${srcdir}/src/tests/*
#EXTRA_DIST = share/*
#dist_data_DATA = share/*

get:
	rsync -avuzb --exclude '.svn' /export/${USER}/work/zmutil/ .

put:
	rsync -Cavuzb . /export/${USER}/work/zmutil/

sync: get put

