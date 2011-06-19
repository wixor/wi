#!/usr/bin/env python
# -*- coding: utf-8

import sys
import re
import subprocess


if len(sys.argv) != 5:
	print "usage: ./interesting_answers.py wzorzec_dla_wi.txt alfa beta gamma > odpowiedzi.txt"
	print "\talfa  - 'tf-idf' coefficient"
	print "\tbeta  - 'pagerank' coefficient"
	print "\tgamma - 'tf-idf * pagerank' coefficient"
	exit(1)

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
#		sys.stderr.write(j.encode('utf-8') + '\t',)
#	sys.stderr.write('\n')

p = subprocess.Popen(['./search -m -v -f %s:%s:%s' % (sys.argv[2], sys.argv[3], sys.argv[4])], shell=True, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
result = p.communicate(query.encode('utf-8'))

answer_pattern = re.compile(r"\d+: (.+) (\([^(]+\))", re.U)
i = 0
verbose_staff = True
for raw_line in result[0].split('\n'):
	line = unify(raw_line.decode('utf-8'))
	if line[:3] == '---':
		i += 1
		verbose_staff = True
		print
	elif line[:5] == 'query':
		print raw_line#line.encode('utf-8')
		verbose_staff = False
	else:
		if verbose_staff:
			continue
		else:
			just_title = answer_pattern.search(line).group(1)
			if just_title.strip() in interesting_answers[i]:
				print raw_line#line.encode('utf-8')
	
