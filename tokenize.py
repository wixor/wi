#!/usr/bin/env python
# -*- coding: utf-8 -*-

# Mateusz Dereniowski
# Wyszukiwanie informacji lato 2011

import sys, os, codecs
from struct import pack
import re
import string

def tokenize(filename):
	"""Tokenizing given text,\nTitles starts with tag: "##TITLE## "."""

	dummy_word = '"'
	title_pattern = re.compile(r"^##TITLE## (.*)$")
	diacritics = r"ąęśćżźńółèüäéáúūōíñõ"# ąęśćżźńółÀ-öø-ÿĀ-ſ
	unwanted_char = re.compile(r"[^- a-z0-9{0}_.']".format(diacritics))
	symbols = r"""[,'"!?:;@#$%^&*()+=[\]{}\|<>/]"""
	unwanted_symbols = re.compile(r"(?:{0})|(?:\B[-.']\B)|(?:(?<=\d)[-'.](?=\d))|(?:\B[-.']\b)|(?:\b[-.']\B)".format(symbols))
	parenthesis = re.compile(r"[()]")
	spaces = re.compile(r"\s+")
	only_spaces = re.compile(r"^\s+$")
	#lower = string.maketrans(r"ABCDEFGHIJKLMNOPQRSTUVWXYZĄĘŚĆŻŹŃÓŁÈÜÄÉÁÚŪŌÍÑÕ", r"abcdefghijklmnopqrstuvwxyząęśćżźńółèüäéáúūōíñõ")

	tytuly_count = 0
	### reading and parsing file ###
	with open(filename, 'r', buffering=2**27) as f_SRC:
	 with open('TITL-'+os.path.basename(filename), 'w', buffering=2**25) as f_TITL:
	  with open('TOKENS-'+os.path.basename(filename), 'w', buffering=2**25) as f_TOKENS:
		f_TITL.write('TITL')
		f_TITL.seek(4, 1)
		i = 0
		for line in f_SRC:
			title = title_pattern.match(line)
			if title:
				if tytuly_count != 0:
					f_TOKENS.write('\n')
				tytuly_count += 1
				tytul = title.group(1).strip()
				f_TITL.write(tytul + "\x00")
				#tytul = tytul.translate(lower)
				tytul = tytul.decode('utf8').lower().encode('utf8')
				tytul = re.sub(parenthesis, r"", tytul)
				f_TOKENS.write(' '.join((tytul, dummy_word)))
			else:
				#line = line.translate(lower)
				line = line.decode('utf8').lower().encode('utf8')
				line = re.sub(unwanted_symbols, r" ", line)
				if not re.match(only_spaces, line):
					tokens = re.split(spaces, line.strip())
					for token in tokens:
						if unwanted_char.search(token):
							f_TOKENS.write(' ' + dummy_word)
						else:
							f_TOKENS.write(' ' + token)
		f_TOKENS.write('\n')

	 with open('TITL-'+os.path.basename(filename), 'rb+') as f_TITL:
		f_TITL.seek(4, 0)
		f_TITL.write(pack('<I', tytuly_count))

def main():
	if len(sys.argv) < 2:
		print 'usage: ./tokenize.py wiki_src'
		exit(1)

	tokenize(sys.argv[1])


if __name__ == "__main__":
	main()

