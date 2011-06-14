#!/usr/bin/env python
# -*- coding: utf-8 -*-

# Mateusz Dereniowski, Wiktor Janas
# Wyszukiwanie informacji lato 2011

import sys, os
from struct import pack
import re
import array
import marshal

def wikilinks_graph():
	colon = re.compile(r': ')
	def unify_title(title):
		title = title.strip().replace('_', ' ')
		title = re.sub(colon, ':', title)
		return title

	sys.stderr.write("Loading previously extracted articles' titles from wikipedia.\n")
	with open('db/articles.marshal', 'rb') as f_ARTICLES:
		articles_titles = marshal.load(f_ARTICLES)
	
	title_to_number = {}
	print "Articles with double entries:"
	for i, title in articles_titles:
		lower_title = title.lower()
		if lower_title in title_to_number:
			print title
		else:
			title_to_number[lower_title] = i
	
	titles_count = len(articles_titles)
	outgoing_links = [None] * titles_count

	with open(wikilinks, 'rb', buffering=2**27) as f_WIKILINKS:
		first_title = unify_title(f_WIKILINKS.readline().decode('utf8')).lower()
		if first_title in title_to_number:
			current_title_nb = title_to_number[first_title]

		for line in f_WIKILINKS:
			title = unify_title(line.decode('utf8')).lower()
			if line[0] != ' ': # it's line with title
				if title in title_to_number:
					current_title_nb = title_to_number[title]
				else:
					current_title_nb = titles_count
					title_to_number[title]


	### reading all articles titles from 'wikilinki.txt' ###
	if os.path.exists('db/wikilinki.marshal'):
		with open('db/wikilinki.marshal', 'rb') as f:
			sys.stderr.write("Loading previously parsed 'wikilinki.txt'...\n")
			articles_from_links = marshal.load(f)
			sys.stderr.write("...finished.\n")
	else:
		sys.stderr.write("Loading articles' titles from 'wikilinki.txt'...\n")
		articles_from_links = set()
		with open(wikilinks, 'rb', buffering=2**27) as f_WIKILINKS:
			for title in f_WIKILINKS:
				title = title.decode('utf8')
				title = title.strip()
				title = title.lower()
				title = title.replace('_', ' ')
				title = re.sub(colon, ':', title)
				articles_from_links.add(title)
				with open('db/wikilinki.marshal', 'wb') as f:
					marshal.dump(articles_from_links, f)
					sys.stderr.write("...finished.\n")


		print "articles that weren't in 'wikilinki.txt':"
		new_set = set()
		for title in articles_titles:
			lower_title = title.lower()
			if lower_title in articles_from_links:
				articles_from_links.remove(lower_title)
			elif title in new_set: # repeated article title
				sys.stderr.write('repeated\t' + title.encode('utf8') + '\n')
			else:
				print title.encode('utf8')
			new_set.add(title)

		for _ in articles_from_links:
			f_TOKENS.write(dummy_word + '\n')


