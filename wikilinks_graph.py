#!/usr/bin/env python
# -*- coding: utf-8 -*-

# Mateusz Dereniowski, Wiktor Janas
# Wyszukiwanie informacji lato 2011

import sys, os
from struct import pack
import re
import array
import marshal


def wikilinks_graph(wikilinki):
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
	for i, title in enumerate(articles_titles):
		lower_title = title.lower()
		if lower_title in title_to_number:
			print title.encode('utf8')
		else:
			title_to_number[lower_title] = i
	
	titles_count = len(articles_titles)
	outgoing_links = [array.array('I') for _ in xrange(titles_count)]

	sys.stderr.write("Digitalizing wikilinks:\n")
	with open(wikilinki, 'rb', buffering=2**24) as f_WIKILINKS:
		first_title = unify_title(f_WIKILINKS.readline().decode('utf8')).lower()
		if first_title in title_to_number:
			current_title_no = title_to_number[first_title]

		for line in f_WIKILINKS:
			title = unify_title(line.decode('utf8')).lower()
			if line[0] != ' ': # line with title
				if title in title_to_number:
					current_title_no = title_to_number[title]
				else:
					current_title_no = titles_count
					title_to_number[title] = titles_count
					titles_count += 1
					outgoing_links.append(array.array('I'))
			else:
				if title in title_to_number:
					link_no = title_to_number[title]
				else:
					link_no = titles_count
					title_to_number[title] = titles_count
					titles_count += 1
					outgoing_links.append(array.array('I'))

				outgoing_links[current_title_no].append(link_no)

	with open('db/wikilinks', 'wb', buffering=2**24) as f:
		f.write('LINK')
		f.write(pack('<I', titles_count))
		for articles_links in outgoing_links:
			f.write(pack('<H', len(articles_links)))
			for link in articles_links:
				f.write(pack('<I', link))


def main():
	if len(sys.argv) != 2:
		print 'usage: ./wikilinks_graph.py wikilinki.txt > articles_repeated_in_wikipedia'
		exit(1)

	wikilinks_graph(sys.argv[1])


if __name__ == "__main__":
	main()

