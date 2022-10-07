spellcheck = False
                          'recirculation', 'linux', 'afxdp', 'promisc', 'goto',
                          'misconfigured', 'misconfiguration', 'checkpatch',
                          'debian', 'travis', 'cirrus', 'appveyor', 'faq',
                          'erspan', 'const', 'hotplug', 'addresssanitizer',
                          'ovsdb', 'dpif', 'veth', 'rhel', 'jsonrpc', 'json',
                          'syscall', 'lacp', 'ipf', 'skb', 'valgrind']
__regex_hash_define_for_each = re.compile(
    r'#define [_A-Z]+FOR_*EACH[_A-Z0-9]*\(')
__regex_cast_missing_whitespace = re.compile(r'\)[a-zA-Z0-9]')
__regex_nonascii_characters = re.compile("[^\u0000-\u007f]")
__regex_efgrep = re.compile(r'.*[ef]grep.*$')
skip_gerrit_change_id_check = False
    r'\.(am|at|etc|in|m4|mk|patch|py)$|^debian/.*$')
leading_whitespace_blacklist = re.compile(r'\.(mk|am|at)$|^debian/.*$')
    if (__regex_for_if_missing_whitespace.search(line) is not None and
        __regex_hash_define_for_each.search(line) is None):
        return False
    if (__regex_for_if_too_much_whitespace.search(line) is not None or
        __regex_for_if_parens_whitespace.search(line)):
def nonascii_character_check(line):
    """Return TRUE if inappropriate Unicode characters are detected """
    return __regex_nonascii_characters.search(line) is not None


def cast_whitespace_check(line):
    """Return TRUE if there is no space between the '()' used in a cast and
       the expression whose type is cast, i.e.: '(void *)foo'"""
    return __regex_cast_missing_whitespace.search(line) is not None


def has_efgrep(line):
    """Returns TRUE if the current line contains 'egrep' or 'fgrep'."""
    return __regex_efgrep.match(line) is not None


def check_spelling(line, comment):
    if not spell_check_dict or not spellcheck:
    words = filter_comments(line, True) if comment else line
    words = words.replace(':', ' ').split(' ')

    for word in words:
        if (len(strword)
                and not spell_check_dict.check(strword.lower())
                and not spell_check_dict.check(word.lower())):
            # skip words containing numbers
            if any(check_char.isdigit() for check_char in strword):
    {'regex': r'(\.c|\.h)(\.in)?$', 'match_name': None,
     'check': lambda x: nonascii_character_check(x),
     'print':
     lambda: print_error("Inappropriate non-ascii characters detected.")},

    {'regex': r'(\.c|\.h)(\.in)?$', 'match_name': None,
     'prereq': lambda x: not is_comment_line(x),
     'check': lambda x: cast_whitespace_check(x),
     'print':
     lambda: print_error("Inappropriate spacing around cast")},

     'check': lambda x: check_spelling(x, True)},

    {'regex': r'(\.at|\.sh)$', 'match_name': None,
     'check': lambda x: has_efgrep(x),
     'print':
     lambda: print_error("grep -E/-F should be used instead of egrep/fgrep")},
def regex_warn_factory(description):
    return lambda: print_warning(description)



easy_to_misuse_api = [
        ('ovsrcu_barrier',
            'lib/ovs-rcu.c',
            'Are you sure you need to use ovsrcu_barrier(), '
            'in most cases ovsrcu_synchronize() will be fine?'),
        ]

checks += [
    {'regex': r'(\.c)(\.in)?$',
     'match_name': lambda x: x != location,
     'prereq': lambda x: not is_comment_line(x),
     'check': regex_function_factory(function_name),
     'print': regex_warn_factory(description)}
    for (function_name, location, description) in easy_to_misuse_api]

        regex_check = True
        match_check = True

            continue

           re.compile(check['regex']).search(filename) is None:
            regex_check = False

        if check['match_name'] is not None and \
                not check['match_name'](filename):
            match_check = False

        if regex_check and match_check:
    is_fixes = re.compile(r'(\s*(Fixes:)(.*))$', re.I | re.M | re.S)
    is_fixes_exact = re.compile(r'^Fixes: [0-9a-f]{12} \(".*"\)$')

    tags_typos = {
        r'^Acked by:': 'Acked-by:',
        r'^Reported at:': 'Reported-at:',
        r'^Reported by:': 'Reported-by:',
        r'^Requested by:': 'Requested-by:',
        r'^Reviewed by:': 'Reviewed-by:',
        r'^Submitted at:': 'Submitted-at:',
        r'^Suggested by:': 'Suggested-by:',
    }
    for line in text.split("\n"):

        if line == "\f":
            # Form feed
            continue
            elif (is_gerrit_change_id.match(line) and
                  not skip_gerrit_change_id_check):
            elif is_fixes.match(line) and not is_fixes_exact.match(line):
                print_error('"Fixes" tag is malformed.\n'
                            'Use the following format:\n'
                            '  git log -1 '
                            '--pretty=format:"Fixes: %h (\\\"%s\\\")" '
                            '--abbrev=12 COMMIT_REF\n')
                print("%d: %s\n" % (lineno, line))
            elif spellcheck:
                check_spelling(line, False)
            for typo, correct in tags_typos.items():
                m = re.match(typo, line, re.I)
                if m:
                    print_error("%s tag is malformed." % (correct[:-1]))
                    print("%d: %s\n" % (lineno, line))

            if current_file.startswith('utilities/bugtool'):
                continue
-S|--spellcheck                Check C comments and commit-message for possible
                               spelling mistakes
-t|--skip-trailing-whitespace  Skips the trailing whitespace test
   --skip-gerrit-change-id     Skips the gerrit change id test"""
        mail = email.message_from_file(open(filename, 'r', encoding='utf8'))
                                       "skip-gerrit-change-id",
                                       "spellcheck",
        elif o in ("--skip-gerrit-change-id"):
            skip_gerrit_change_id_check = True
        elif o in ("-S", "--spellcheck"):
                spellcheck = True