#!/usr/bin/env python
# -*- coding: utf-8 -*-

# Mateusz Dereniowski, Wiktor Janas
# Wyszukiwanie informacji lato 2011

import sys
import re

word = ur"""a-zß-öø-ÿĀ-ſ"""
unwanted_char = re.compile(ur"[^-{word}0-9.'\"]".format(word=word), re.U)

def check_unwanted_chars(filename):
	with open(filename, 'rb') as f:
		for i, line in enumerate(f):
			title = True
			line = line.decode('utf8')
			for j, token in enumerate(line.split()):
				if token == u'"':
					title = False
				if not title:
					m = re.findall(unwanted_char, token)
					if m:
						for c in m:
							print 'Line:', i, 'token number:', j, ';\tunexpected character:', "'" + c + "'", '\trepresentation: ', repr(c)

def main():
	if len(sys.argv) < 2:
		print 'usage: ./check_unwanted_chars.py tokenized_wiki'
		exit(1)

	check_unwanted_chars(sys.argv[1])

if __name__ == '__main__':
	main()
	
