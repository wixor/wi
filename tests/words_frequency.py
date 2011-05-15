#!/usr/bin/env python
# -*- coding: utf-8 -*-

# Mateusz Dereniowski, Wiktor Janas
# Wyszukiwanie informacji lato 2011

import sys
import os
import operator

def status(s):
	sys.stderr.write(s + "\r")

dummy_word = '"'

def count_words_freq(filename):
	"""Scans through file of tokenized Wikipedia documents, counting distinct token's frequency."""
	words_freq = {}
	words_freq_per_article = {}
	tokens_count = 0
	distinct_words = 0
	dummy_words_count = 0
	### reading tokens ###
	with open(filename, 'rb', buffering=2**25) as f_TOKENS:
		for i, line in enumerate(f_TOKENS):
			status('Processing article {0}'.format(i))
			dummy_words_count -= 1 # title is separated from text with a dummy word
			s = set()
			for token in line.split():
				if token != dummy_word:
					tokens_count += 1
					if token in words_freq:
						words_freq[token] += 1
					else:
						words_freq[token] = 1
						distinct_words += 1
					if token not in s:
						words_freq_per_article[token] = words_freq_per_article.get(token, 0) + 1
						s.add(token)
				else:
					dummy_words_count += 1
		status('Processed {0} articles.\n'.format(i+1))

	sys.stderr.write("Our tokenizer distinguished {0} tokens with known characters and {1} with unknown. There were {2} distinct, correct tokens.".format(tokens_count, dummy_words_count, distinct_words))

	words_freq_sorted = sorted(words_freq.items(), key=lambda (k, v): (v, k))
	with open(os.path.dirname(filename) + '/words_frequency', 'wb', buffering=2**25) as f:
		for word, freq in words_freq_sorted:
			f.write(word + '\t' + str(freq) + '\n')

	words_freq_sorted = sorted(words_freq_per_article.items(), key=lambda (k, v): (v, k))
	with open(os.path.dirname(filename) + '/words_frequency_per_article', 'wb', buffering=2**25) as f:
		for word, freq in words_freq_sorted:
			f.write(word + '\t' + str(freq) + '\n')


def main():
	if len(sys.argv) < 2:
		print 'usage: ./words_freq_frequency.py tokenized_wiki'
		exit(1)

	count_words_freq(sys.argv[1])


if __name__ == "__main__":
	main()

