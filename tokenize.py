#!/usr/bin/env python
# -*- coding: utf-8 -*-

# Mateusz Dereniowski
# Wyszukiwanie informacji lato 2011

import sys, os
from struct import pack
import re
import string
try:
	import tools
	dummy_word = tools.dummy_word
except ImportError:
	dummy_word = r'"'

def tokenize(filename):
	"""Tokenizing given text,\nTitles starts with tag: "##TITLE## "."""

	title_pattern = re.compile(r"^##TITLE## (.*)$")
	word = ur"""[a-zß-öø-ÿĀ-ſ]"""
	not_word = ur"""[^a-zß-öø-ÿĀ-ſ]""" # ąęśćżźńółèüäéáúūōíñõ
	unwanted_char = re.compile(ur"{0}[^-0-9.']".format(not_word), re.U)
	symbols = ur"""[,"!?:;@#$%^&*()+_=[\]{}\|<>/]"""
	unwanted_symbol = re.compile(ur"(?:{symbols})|(?:(?<={not_word})[-.'])|(?:[-.'](?={not_word}))|(?:^[-.'])|(?:[-.']$)".format(symbols=symbols, not_word=not_word), re.U)
	parenthesis = re.compile(ur"[()]", re.U)

	tytuly_count = 0
	### reading and parsing file ###
	with open(filename, 'rb', buffering=2**27) as f_SRC:
	 with open('db/TITL-'+os.path.basename(filename), 'wb', buffering=2**25) as f_TITL:
	  with open('db/TOKENS-'+os.path.basename(filename), 'wb', buffering=2**25) as f_TOKENS:
		f_TITL.write('TITL')
		f_TITL.seek(4, 1)

		for line in f_SRC:
			line = line.decode('utf8')
			title = title_pattern.match(line)
			if title:
				if tytuly_count != 0:
					f_TOKENS.write('\n')
				tytuly_count += 1
				tytul = title.group(1).strip()
				f_TITL.write(tytul.encode('utf8') + "\x00")
				tytul = tytul.lower()
				tytul = re.sub(parenthesis, ur"", tytul)
				f_TOKENS.write(' '.join((tytul.encode('utf8'), dummy_word)))
			else:
				line = line.lower()
				line = re.sub(unwanted_symbol, ur" ", line)
				tokens = line.split()
				for token in tokens:
					if unwanted_char.search(token):
						f_TOKENS.write(' ' + dummy_word)
					else:
						f_TOKENS.write(' ' + token.encode('utf8'))
		f_TOKENS.write('\n')

	 with open('db/TITL-'+os.path.basename(filename), 'rb+') as f_TITL:
		f_TITL.seek(4, 0)
		f_TITL.write(pack('<I', tytuly_count))

def main():
	if len(sys.argv) < 2:
		print 'usage: ./tokenize.py wiki_src'
		exit(1)

	tokenize(sys.argv[1])


if __name__ == "__main__":
	main()

