#!/usr/bin/env python
# -*- coding: utf-8

import sys
import re
import subprocess


def unify(s):
	return (' '.join(s.strip().split())).lower()


query = u""
interesting_answers = []
f = open(sys.argv[1], 'r')
interesting = set()
for line in f:
	line = unify(line.decode('utf-8'))
	if len(line) == 0:
		interesting_answers.append(interesting)
		interesting = set()
	elif line[:5] == 'query':
		query += line[7:] + '\n'
	elif line == '{' or line == '}':
		continue
	else:
		interesting.add(line)
f.close()

#for i in interesting_answers:
#	for j in i:
#		print j + '\t',
#	print

p = subprocess.Popen(['./search -m -v -f %s:%s:%s' % (sys.argv[2], sys.argv[3], sys.argv[4])], shell=True, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
result = p.communicate(query.encode('utf-8'))

answer_pattern = re.compile(r"\d+: (.+) (\([^(]+\))", re.U)
i = 0
verbose_staff = True
for line in result[0].split('\n'):
	line = unify(line)
	if line[:3] == '---':
		i += 1
		verbose_staff = True
	elif line[:5] == 'query':
		print line
		verbose_staff = False
	else:
		if verbose_staff:
			continue
		else:
			just_title = answer_pattern.search(line).group(1)
			if just_title in interesting_answers[i]:
				print line
	
