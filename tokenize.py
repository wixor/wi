#!/usr/bin/env python
# -*- coding: utf-8 -*-

# Mateusz Dereniowski
# Wyszukiwanie informacji lato 2011

import sys, os, codecs
from struct import pack
import re

def parse(filename):
	"""Parsing given text,\nTitles starts with tag: "##TITLE## "."""

	dummy_word = '"'
	match_title = re.compile(ur"^##TITLE## (.*)$", flags=re.UNICODE)
	not_polish_word = re.compile(ur"[^-\.a-ząęśćżźńół0-9]", flags=re.UNICODE)

	tytuly_count = 0
	line_count = 0
	### reading and parsing file ###
	with codecs.open(filename, 'r', encoding='utf-8', buffering=2**25) as f_SRC:
	 with codecs.open('TITL-'+os.path.basename(filename), 'w', encoding='utf-8', buffering=2**25) as f_TITL:
	  with codecs.open('TOKENS-'+os.path.basename(filename), 'w', encoding='utf-8', buffering=2**25) as f_TOKENS:
		f_TITL.write('TITL')
		f_TITL.seek(4, 1)

		for line in f_SRC:
			title = match_title.match(line)
			if title:
				if tytuly_count != 0:
					f_TOKENS.write('\n')
				tytuly_count += 1
				tytul = title.group(1).strip().lower()
				f_TITL.write(tytul + "\x00")
				f_TOKENS.write(' '.join((tytul, dummy_word)))
			else:
				# removing all characters different than '.', '-', letters and digits
				line = re.sub(ur"([^-.\w\d]+)", ur" ", line, flags=re.UNICODE)
				# preserving words with dash and middle dots of multi-word abbreviations like 'e.g', 'p.n.e'
				line = re.sub(ur"(?:\s-\s)|(?:\.(?:\s|$))", ur" ", line, flags=re.UNICODE)
				# striping and converting to lowercase
				line = line.strip().lower()
				# tokenizing by spliting received line
				tokens = re.split(ur"\s+", line, flags=re.UNICODE)
				for token in tokens:
					if token != u'':
						if not_polish_word.search(token):
							f_TOKENS.write(' ' + dummy_word)
						else:
							f_TOKENS.write(' ' + token)
			line_count += 1
		f_TOKENS.write('\n')

	 with open('TITL-'+os.path.basename(filename), 'rb+') as f_TITL:
		f_TITL.seek(4, 0)
		f_TITL.write(pack('<I', tytuly_count))

def main():
	if len(sys.argv) < 2:
		print 'usage: ./tokenize.py wiki_src'
		exit(1)

	parse(sys.argv[1])


if __name__ == "__main__":
	main()

