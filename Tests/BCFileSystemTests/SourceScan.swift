import Foundation

/// The three house rules, checked over Swift source text: source in, findings out, and nothing else touched.
/// `SourceRulesTests` feeds it this package's own files and planted cases.
///
/// The check is lexical. It reads the text with comments and string literals blanked, so a rule named in a
/// comment or quoted in a string is not a finding, while code inside a string interpolation still is.
///
/// What it cannot see, and a review still must:
///   - a default respelled: an overload that forwards with an argument filled in, or `value ?? fallback` in a body;
///   - a reach through a name of one's own (`let files = FileManager.self`), or one not in the vocabulary below;
///   - anything inside a regular-expression literal, which is read as code.
enum SourceScan {
    /// A default in a parameter list is allowed only when it is one of these: the caller's own position, which
    /// only a default can capture.
    static let callerContextCaptures: Set<String> = [
        "#file", "#fileID", "#filePath", "#function", "#line", "#column", "#dsohandle", "#isolation",
        "#_sourceLocation",
    ]

    /// The one file where a reach is legal: the fixtures' temporary directory, the tests' own file system. There
    /// each reach carries a line comment that opens with the marker below and gives a reason; anywhere else the
    /// marker is itself a finding.
    static let reachIsLegalInFileEndingWith = "Sources/BCFileSystemTestFixtures/TemporaryDirectory.swift"

    static let reachMarker = "reach-ok:"

    /// Each reach, as a regular expression over the blanked text, with the name a finding reports. A reach is
    /// code obtaining something from the process around it (the file system, the clock, the environment, an
    /// id, the network, a stream) instead of being handed it.
    static let reachVocabulary: [(name: String, pattern: String)] = [
        ("FileManager.default", #"\bFileManager\s*\.\s*default\b"#),
        ("FileManager()", #"\bFileManager\s*\(\s*\)"#),
        ("NSTemporaryDirectory()", #"\bNSTemporaryDirectory\s*\("#),
        ("NSHomeDirectory()", #"\bNSHomeDirectory\s*\("#),
        ("Data(contentsOf:)", #"\bData\s*\(\s*contentsOf\b"#),
        ("String(contentsOf:)", #"\bString\s*\(\s*contentsOf"#),
        (".write(to:)", #"\.\s*write\s*\(\s*to\s*:"#),
        ("ProcessInfo.processInfo", #"\bProcessInfo\s*\.\s*processInfo\b"#),
        ("getenv()", #"\bgetenv\s*\("#),
        ("CommandLine.arguments", #"\bCommandLine\s*\.\s*arguments\b"#),
        ("Date()", #"\bDate\s*\(\s*\)"#),
        ("Date.now", #"\bDate\s*\.\s*now\b"#),
        ("Date(timeIntervalSinceNow:)", #"\bDate\s*\(\s*timeIntervalSinceNow\b"#),
        ("Task.sleep", #"\bTask\s*\.\s*sleep\b"#),
        ("Thread.sleep", #"\bThread\s*\.\s*sleep\b"#),
        ("UUID()", #"\bUUID\s*\(\s*\)"#),
        (".random(in:)", #"\.\s*random\s*\(\s*in\s*:"#),
        ("SystemRandomNumberGenerator", #"\bSystemRandomNumberGenerator\b"#),
        ("URLSession.shared", #"\bURLSession\s*\.\s*shared\b"#),
        ("UserDefaults.standard", #"\bUserDefaults\s*\.\s*standard\b"#),
        ("Locale.current", #"\bLocale\s*\.\s*current\b"#),
        ("TimeZone.current", #"\bTimeZone\s*\.\s*current\b"#),
        ("Calendar.current", #"\bCalendar\s*\.\s*current\b"#),
        ("Bundle.main", #"\bBundle\s*\.\s*main\b"#),
        ("NotificationCenter.default", #"\bNotificationCenter\s*\.\s*default\b"#),
        ("DispatchQueue.main", #"\bDispatchQueue\s*\.\s*main\b"#),
        ("FileHandle.standard…", #"\bFileHandle\s*\.\s*standard(Output|Error|Input)\b"#),
        ("print()", #"\bprint\s*\("#),
        ("Process()", #"\bProcess\s*\(\s*\)"#),
        ("exit()", #"\bexit\s*\("#),
        // The same reaches written as implicit members, which name no type: `date: { .now }`, `session: .shared`.
        // The member must open an expression, so `Type.shared` is left to its own line above.
        (
            "an implicit .now, .current, .shared, .standard or .default",
            #"(?<![\w\)\]\}\?\!])\.\s*(now|current|shared|standard|default)\b"#
        ),
    ]

    // MARK: - Blanking

    private enum Mode {
        /// Code. Inside a string interpolation, `interpolationDepth` counts the parentheses still open.
        case code(interpolationDepth: Int?)
        case lineComment
        /// Swift's block comments nest, so the depth is counted.
        case blockComment(depth: Int)
        /// A string literal: `hashes` is the number of `#` in its delimiter, `isMultiline` whether it is `"""`.
        case string(hashes: Int, isMultiline: Bool)
    }

    /// Two views of one source text, each the same length and line layout as the source, so that a position in
    /// either is the same line and column in the source.
    struct Views {
        /// The code: every comment and every string literal replaced by spaces. Code inside `\( )` in a string
        /// stays.
        let code: String
        /// The line comments, `//` included, with everything else replaced by spaces. This is where a marker is
        /// read from, so that marker text quoted in a string or a block comment is not taken for one.
        let lineComments: String
    }

    static func views(of source: String) -> Views {
        let characters = Array(source)
        var code: [Character] = []
        var lineComments: [Character] = []
        code.reserveCapacity(characters.count)
        lineComments.reserveCapacity(characters.count)
        var modes: [Mode] = [.code(interpolationDepth: nil)]
        var index = 0

        func matches(_ text: String, at position: Int) -> Bool {
            let wanted = Array(text)
            guard position + wanted.count <= characters.count else { return false }
            return Array(characters[position..<(position + wanted.count)]) == wanted
        }
        func blanked(_ character: Character) -> Character {
            character.isNewline ? character : " "
        }
        /// Moves past `count` characters that are neither code nor line comment: blank in both views.
        func blank(_ count: Int) {
            for offset in 0..<count {
                code.append(blanked(characters[index + offset]))
                lineComments.append(blanked(characters[index + offset]))
            }
            index += count
        }
        /// Moves past one character of code.
        func keep() {
            code.append(characters[index])
            lineComments.append(blanked(characters[index]))
            index += 1
        }
        /// Moves past `count` characters of a line comment.
        func keepAsLineComment(_ count: Int) {
            for offset in 0..<count {
                code.append(blanked(characters[index + offset]))
                lineComments.append(characters[index + offset])
            }
            index += count
        }
        /// The length of a string-opening delimiter at `position` (`"`, `"""`, `#"`, `##"""`, …), with its
        /// shape, or nil when no string opens there.
        func stringOpening(at position: Int) -> (length: Int, hashes: Int, isMultiline: Bool)? {
            var hashes = 0
            while position + hashes < characters.count, characters[position + hashes] == "#" {
                hashes += 1
            }
            let quote = position + hashes
            guard quote < characters.count, characters[quote] == "\"" else { return nil }
            if matches("\"\"\"", at: quote) {
                return (hashes + 3, hashes, true)
            }
            return (hashes + 1, hashes, false)
        }

        while index < characters.count {
            let character = characters[index]
            switch modes[modes.count - 1] {
            case .code(let interpolationDepth):
                if matches("//", at: index) {
                    modes.append(.lineComment)
                    keepAsLineComment(2)
                } else if matches("/*", at: index) {
                    modes.append(.blockComment(depth: 1))
                    blank(2)
                } else if let opening = stringOpening(at: index) {
                    modes.append(.string(hashes: opening.hashes, isMultiline: opening.isMultiline))
                    blank(opening.length)
                } else if let depth = interpolationDepth, character == "(" {
                    modes[modes.count - 1] = .code(interpolationDepth: depth + 1)
                    keep()
                } else if let depth = interpolationDepth, character == ")" {
                    if depth == 0 {
                        // The parenthesis that closes the interpolation: back into the string.
                        modes.removeLast()
                        blank(1)
                    } else {
                        modes[modes.count - 1] = .code(interpolationDepth: depth - 1)
                        keep()
                    }
                } else {
                    keep()
                }

            case .lineComment:
                if character.isNewline {
                    modes.removeLast()
                    blank(1)
                } else {
                    keepAsLineComment(1)
                }

            case .blockComment(let depth):
                if matches("/*", at: index) {
                    modes[modes.count - 1] = .blockComment(depth: depth + 1)
                    blank(2)
                } else if matches("*/", at: index) {
                    if depth == 1 {
                        modes.removeLast()
                    } else {
                        modes[modes.count - 1] = .blockComment(depth: depth - 1)
                    }
                    blank(2)
                } else {
                    blank(1)
                }

            case .string(let hashes, let isMultiline):
                let escape = "\\" + String(repeating: "#", count: hashes)
                let closing = (isMultiline ? "\"\"\"" : "\"") + String(repeating: "#", count: hashes)
                if matches(escape + "(", at: index) {
                    modes.append(.code(interpolationDepth: 0))
                    blank(escape.count + 1)
                } else if matches(escape, at: index), index + escape.count < characters.count {
                    // An escaped character, which may be the quote itself.
                    blank(escape.count + 1)
                } else if matches(closing, at: index) {
                    modes.removeLast()
                    blank(closing.count)
                } else {
                    blank(1)
                }
            }
        }
        return Views(code: String(code), lineComments: String(lineComments))
    }

    // MARK: - Default arguments

    /// One finding per parameter default in a `func`, `init`, `subscript`, `macro` or enum `case` declaration of
    /// `source`, except the caller-context captures. `path` only labels the findings.
    static func defaultArgumentFindings(source: String, path: String) -> [String] {
        let characters = Array(views(of: source).code)
        var findings: [String] = []
        var index = 0
        while index < characters.count {
            guard let keyword = declarationKeyword(in: characters, at: index) else {
                index += 1
                continue
            }
            index += keyword.count
            var lists: [Range<Int>] = []
            if keyword == "case" {
                // An enum case may declare several names on one line, each with its own list.
                lists = parameterListsOfCaseStatement(in: characters, from: index)
            } else if let list = parameterListOfDeclaration(in: characters, from: index) {
                lists = [list]
            }
            for list in lists {
                for defaultValue in defaultValues(in: characters, list: list) {
                    if callerContextCaptures.contains(defaultValue.expression) {
                        continue
                    }
                    let line = lineNumber(of: defaultValue.position, in: characters)
                    findings.append(
                        "\(path):\(line): default argument: `= \(defaultValue.expression)` lets a caller omit the value"
                    )
                }
            }
        }
        return findings
    }

    /// The declaration keyword that starts at `index`, as a whole word that is not a member access (`.init(`
    /// is a call, not a declaration).
    private static func declarationKeyword(in characters: [Character], at index: Int) -> String? {
        if index > 0 {
            let previous = characters[index - 1]
            if isIdentifierCharacter(previous) || previous == "." || previous == "#" || previous == "`" {
                return nil
            }
        }
        for keyword in ["func", "init", "subscript", "macro", "case"] {
            let end = index + keyword.count
            guard end <= characters.count, String(characters[index..<end]) == keyword else { continue }
            if end < characters.count, isIdentifierCharacter(characters[end]) {
                continue
            }
            return keyword
        }
        return nil
    }

    /// The parenthesised parameter list of a `func`, `init`, `subscript` or `macro` whose keyword ends at
    /// `start`: the range between its parentheses. Nil when a body, a statement end or the end of the text
    /// comes first.
    private static func parameterListOfDeclaration(in characters: [Character], from start: Int) -> Range<Int>? {
        var index = start
        while index < characters.count {
            let character = characters[index]
            if character == "(" {
                return balancedGroup(in: characters, openingAt: index)
            }
            if character == "{" || character == "}" || character == ";" {
                return nil
            }
            index += 1
        }
        return nil
    }

    /// Every parenthesised list that directly follows a name in the `case` statement starting at `start`. In an
    /// enum these are the associated values; in a `switch` or an `if case` they are patterns, which hold no
    /// `=` of their own and so yield no finding.
    private static func parameterListsOfCaseStatement(in characters: [Character], from start: Int) -> [Range<Int>] {
        var lists: [Range<Int>] = []
        var index = start
        while index < characters.count {
            let character = characters[index]
            if character.isNewline || character == ";" || character == "{" || character == "}" || character == ":" {
                break
            }
            if character == "(" {
                guard let group = balancedGroup(in: characters, openingAt: index) else { break }
                if index > start, isIdentifierCharacter(characters[index - 1]) || characters[index - 1] == "`" {
                    lists.append(group)
                }
                index = group.upperBound + 1
                continue
            }
            index += 1
        }
        return lists
    }

    /// The range strictly inside the parentheses that open at `openingAt`, or nil when they never close.
    private static func balancedGroup(in characters: [Character], openingAt: Int) -> Range<Int>? {
        var depth = 0
        var index = openingAt
        while index < characters.count {
            if characters[index] == "(" {
                depth += 1
            } else if characters[index] == ")" {
                depth -= 1
                if depth == 0 {
                    return (openingAt + 1)..<index
                }
            }
            index += 1
        }
        return nil
    }

    /// Each default in a parameter list: an `=` directly inside the list (not inside a nested bracket, and not
    /// part of `==`, `>=` and the like), with the expression after it up to the next parameter.
    private static func defaultValues(
        in characters: [Character],
        list: Range<Int>
    ) -> [(position: Int, expression: String)] {
        var defaults: [(position: Int, expression: String)] = []
        var nesting = 0
        var index = list.lowerBound
        while index < list.upperBound {
            let character = characters[index]
            if "([{".contains(character) {
                nesting += 1
            } else if ")]}".contains(character) {
                nesting -= 1
            } else if character == "=", nesting == 0, isAssignment(in: characters, at: index) {
                var end = index + 1
                var inner = 0
                while end < list.upperBound {
                    let next = characters[end]
                    if "([{".contains(next) {
                        inner += 1
                    } else if ")]}".contains(next) {
                        inner -= 1
                    } else if next == ",", inner == 0 {
                        break
                    }
                    end += 1
                }
                let expression = String(characters[(index + 1)..<end]).trimmingCharacters(in: .whitespacesAndNewlines)
                defaults.append((index, expression))
                index = end
                continue
            }
            index += 1
        }
        return defaults
    }

    /// Whether the `=` at `index` stands alone, and is not one character of `==`, `!=`, `<=`, `>=` or a compound
    /// assignment.
    private static func isAssignment(in characters: [Character], at index: Int) -> Bool {
        let operatorCharacters = "=!<>+-*/%&|^~?"
        if index > 0, operatorCharacters.contains(characters[index - 1]) {
            return false
        }
        if index + 1 < characters.count, characters[index + 1] == "=" {
            return false
        }
        return true
    }

    // MARK: - Macro declarations

    /// One finding per `macro` declaration in `source`: the keyword as a whole word followed by a name. A
    /// `#externalMacro(...)` expansion, `#expect(...)` and the word in a comment or a string are not
    /// declarations. Authoring a macro is forbidden in every language (AGENTS.md, "No macros").
    static func macroDeclarationFindings(source: String, path: String) -> [String] {
        let characters = Array(views(of: source).code)
        var findings: [String] = []
        var index = 0
        while index < characters.count {
            guard declarationKeyword(in: characters, at: index) == "macro" else {
                index += 1
                continue
            }
            // A declaration names the macro right after the keyword; anything else is the word used otherwise.
            var next = index + "macro".count
            while next < characters.count, characters[next] == " " || characters[next] == "\t" {
                next += 1
            }
            if next < characters.count, next > index + "macro".count, isIdentifierCharacter(characters[next]) {
                let line = lineNumber(of: index, in: characters)
                findings.append("\(path):\(line): macro declaration: a macro is authored here; write a function")
            }
            index = next
        }
        return findings
    }

    // MARK: - Reaches

    /// One finding per reach in `source` that is not a marked line of the fixtures' temporary-directory file, and
    /// one per marker that is out of place or gives no reason. `path` decides where a reach is legal.
    static func reachFindings(source: String, path: String) -> [String] {
        let sourceViews = views(of: source)
        let codeLines = sourceViews.code.components(separatedBy: "\n")
        let commentLines = sourceViews.lineComments.components(separatedBy: "\n")
        let reachIsLegalHere = path.hasSuffix(reachIsLegalInFileEndingWith)
        var findings: [String] = []

        for (lineIndex, codeLine) in codeLines.enumerated() {
            let lineNumber = lineIndex + 1
            let marker = markerReason(inLineComment: commentLines[lineIndex])

            if marker != nil, !reachIsLegalHere {
                findings.append(
                    "\(path):\(lineNumber): reach marker: `\(reachMarker)` is valid only in "
                        + reachIsLegalInFileEndingWith
                )
            }
            if let reason = marker, reason.isEmpty {
                findings.append("\(path):\(lineNumber): reach marker: `\(reachMarker)` needs a reason after it")
            }

            let lineIsExempt = reachIsLegalHere && marker.map { !$0.isEmpty } == true
            if lineIsExempt {
                continue
            }
            for reach in reachVocabulary where lineMatches(codeLine, pattern: reach.pattern) {
                findings.append(
                    "\(path):\(lineNumber): reach: \(reach.name) — take the value or the operation as a parameter"
                )
            }
        }
        return findings
    }

    /// The reason a marker gives, trimmed; an empty string when it gives none; nil when the line has no marker.
    /// `commentLine` is one line of the line-comment view: blank, or a line comment at its own column. A marker
    /// is a line comment that opens with the marker text, so a comment that merely mentions it is not one.
    private static func markerReason(inLineComment commentLine: String) -> String? {
        let comment = commentLine.trimmingCharacters(in: .whitespaces)
        guard comment.hasPrefix("//") else {
            return nil
        }
        let text = comment.dropFirst(2).trimmingCharacters(in: .whitespaces)
        guard text.hasPrefix(reachMarker) else {
            return nil
        }
        return text.dropFirst(reachMarker.count).trimmingCharacters(in: .whitespaces)
    }

    private static func lineMatches(_ line: String, pattern: String) -> Bool {
        line.range(of: pattern, options: .regularExpression) != nil
    }

    // MARK: - Shared

    private static func isIdentifierCharacter(_ character: Character) -> Bool {
        character.isLetter || character.isNumber || character == "_"
    }

    private static func lineNumber(of position: Int, in characters: [Character]) -> Int {
        var line = 1
        for index in 0..<position where characters[index].isNewline {
            line += 1
        }
        return line
    }
}
