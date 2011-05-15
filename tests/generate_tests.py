#!/usr/bin/env python
# -*- coding: utf-8 -*-

# Mateusz Dereniowski, Wiktor Janas
# Wyszukiwanie informacji lato 2011

import sys
import os
import subprocess
from random import random
import marshal
import re

dummy_word = '"'

def generate_tests(path, n):
	if n > 1000:
		n = 1000
	words_freq = {}

	if os.path.exists(path + '/tests/word_freq.marshal'):
		with open(path + '/tests/word_freq.marshal', 'rb') as f:		
			words_freq = marshal.load(f)
	else:
		with open(path + '/db/words_frequency_per_article', 'rb') as f:
			for line in f:
				[word, freq] = line.split()
				if re.match(r'^[\w\d]+$', word):
					words_freq[word] = int(freq)
		with open(path + '/tests/word_freq.marshal', 'wb') as f:		
			marshal.dump(words_freq, f)

	words_list = list(words_freq)

	p = subprocess.Popen([path + '/search', '-r'], bufsize=2**28, cwd=path, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
	questions = []
	for i in xrange(n):
		k = int(random() * (len(words_list) - 1))
		questions.append(words_list[k])

	for q in questions:
		p.stdin.write(q + '\n')
	
	all_correct = True
	answers = p.communicate()[0].split('\n')
	del answers[-1:]
	for i, answer in enumerate(answers):
		[_, question, _, total] = answer.split()
		if int(total) < words_freq[questions[i]]:
			all_correct = False
			print "search found {0} articles matching phrase query {1} and it should be {2}".format(total, question, words_freq[questions[i]])
	if all_correct:
		print "All boolean queries where correctly searched."
#
#	p = subprocess.Popen([path + '/search', '-r'], cwd=path, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
#	questions = []
#	with open(path + '/db/tokenized', 'rb') as f:
#		test_number = 0
#		for i, line in enumerate(f):
#			if test_number == n:
#				break
#			splited_line = line.split()
#			for j, token in enumerate(splited_line):
#				if token == dummy_word:
#					continue
#				elif token in words_freq and words_freq[token] == 1:
#					if j < len(splited_line) - 2 and splited_line[j+1] != dummy_word and splited_line[j+2] != dummy_word:
#						questions.append('"' + token + ' ' + splited_line[j+1] + ' ' +  splited_line[j+2] + '"')
#						test_number += 1
#
#	for q in questions:
#		print q
#		p.stdin.write(q + '\n')
#
#	all_correct = True
#	answers = p.communicate()[0].split('\n')
#	del answers[-1:]
#	total = re.compile(r"TOTAL: (\d+)")
#	for answer in answers:
#		match = re.search(total, answer)
#		if not match:
#			print answer
#		if int(match.group(1)) > 10:
#			all_correct = False
#			print "search didn't found query '{0}'".format(questions)
#	if all_correct:
#		print "All phrase queries where correctly searched."

def main():
	if len(sys.argv) < 3:
		print 'usage: ./generate_tests.py path_to_search number_of_tests'
		exit(1)

	generate_tests(os.path.dirname(sys.argv[1]), int(sys.argv[2]))


if __name__ == "__main__":
	main()

