spellcheck_comments = False
                          'recirculation']
    r'\.(am|at|etc|in|m4|mk|patch|py|dl)$|debian/rules')
leading_whitespace_blacklist = re.compile(
    r'\.(mk|am|at)$|debian/rules|\.gitmodules$')
    if (__regex_for_if_missing_whitespace.search(line) is not None or
            __regex_for_if_too_much_whitespace.search(line) is not None or
            __regex_for_if_parens_whitespace.search(line)):
def check_comment_spelling(line):
    if not spell_check_dict or not spellcheck_comments:
    comment_words = filter_comments(line, True).replace(':', ' ').split(' ')
    for word in comment_words:
        if len(strword) and not spell_check_dict.check(strword.lower()):
            # skip words that start with numbers
            if strword.startswith(tuple('0123456789')):
     'check': lambda x: check_comment_spelling(x)},
           re.compile(check['regex']).search(filename) is not None:
            checkList.append(check)
        elif check['match_name'] is not None and check['match_name'](filename):
    for line in text.splitlines():
            elif is_gerrit_change_id.match(line):
-S|--spellcheck-comments       Check C comments for possible spelling mistakes
-t|--skip-trailing-whitespace  Skips the trailing whitespace test"""
        mail = email.message_from_file(open(filename, 'r'))
                                       "spellcheck-comments",
        elif o in ("-S", "--spellcheck-comments"):
                spellcheck_comments = True