import BCFileSystemTestFixtures
import Testing

/// The guard for the three house rules over this package's Swift, run by `swift test`:
///
///   - no default arguments: no declaration lets a caller omit a value;
///   - inject, never reach: nothing obtains the file system, the clock, the environment or an id from the
///     process around it, outside the fixtures' one legal site;
///   - no macros: no `macro` declaration anywhere.
///
/// The scan is `SourceScan`, a pure function over source text. The first four tests run it over the package; the
/// rest plant each violation in a string, and a near miss beside it, to prove each rule can fail. The package's
/// files are read through the fixtures' legal site, the one place a Swift test touches the file system.
struct SourceRulesTests {
    /// The package's directory, found from this file's own path: `<package>/Tests/BCFileSystemTests/<this file>`.
    private func packageDirectory() -> String {
        var components = #filePath.split(separator: "/", omittingEmptySubsequences: false)
        components.removeLast(3)
        return components.joined(separator: "/")
    }

    /// Every Swift file under `Sources/` and `Tests/`, as (path, text).
    private func sourceAndTestFiles() throws -> [(path: String, text: String)] {
        var files: [(path: String, text: String)] = []
        for top in ["Sources", "Tests"] {
            files += try TemporaryDirectory.textFiles(under: packageDirectory() + "/" + top, suffix: ".swift")
        }
        return files
    }

    // MARK: - The package

    @Test func thePackageDeclaresNoDefaultArguments() throws {
        let findings = try sourceAndTestFiles().flatMap { file in
            SourceScan.defaultArgumentFindings(source: file.text, path: file.path)
        }
        #expect(findings == [], "\(findings.joined(separator: "\n"))")
    }

    @Test func thePackageHoldsNoReachOutsideTheFixturesLegalSite() throws {
        let findings = try sourceAndTestFiles().flatMap { file in
            SourceScan.reachFindings(source: file.text, path: file.path)
        }
        #expect(findings == [], "\(findings.joined(separator: "\n"))")
    }

    @Test func thePackageDeclaresNoMacro() throws {
        let findings = try sourceAndTestFiles().flatMap { file in
            SourceScan.macroDeclarationFindings(source: file.text, path: file.path)
        }
        #expect(findings == [], "\(findings.joined(separator: "\n"))")
    }

    /// A scan that found nothing because it read nothing would pass forever. It must have seen the sources, the
    /// fixtures, the tests, and the one file that holds a marked reach.
    @Test func theScanSawTheSourcesAndTheTests() throws {
        let paths = try sourceAndTestFiles().map(\.path)
        #expect(paths.contains { $0.hasSuffix("Sources/BCFileSystem/FileSystem.swift") }, "\(paths)")
        #expect(
            paths.contains { $0.hasSuffix("Sources/BCFileSystemTestFixtures/ObservedFileSystem.swift") }, "\(paths)")
        #expect(paths.contains { $0.hasSuffix("Tests/BCFileSystemTests/ContractTests.swift") }, "\(paths)")
        #expect(paths.contains { $0.hasSuffix(SourceScan.reachIsLegalInFileEndingWith) }, "\(paths)")
    }

    // MARK: - Default arguments: planted

    private func defaults(in source: String) -> [String] {
        SourceScan.defaultArgumentFindings(source: source, path: "Planted.swift")
    }

    @Test func aFunctionParameterDefaultIsAFinding() {
        let findings = defaults(in: "func write(path: String, durable: Bool = false) {}")
        #expect(findings.count == 1)
        #expect(findings.first?.contains("Planted.swift:1: default argument: `= false`") == true, "\(findings)")
    }

    @Test func anInitialiserDefaultIsAFinding() {
        #expect(defaults(in: "init(clock: @escaping () -> Date = { Date() }) {}").count == 1)
        #expect(defaults(in: "public init?(mode: UInt32 = 0o644) {}").count == 1)
    }

    @Test func aSubscriptDefaultIsAFinding() {
        #expect(defaults(in: "subscript(index: Int, fallback: Int = 0) -> Int { 0 }").count == 1)
    }

    @Test func anEnumAssociatedValueDefaultIsAFinding() {
        #expect(defaults(in: "enum E { case exactly(mode: UInt32 = 0o644) }").count == 1)
        #expect(defaults(in: "enum E { case a(Int), b(retries: Int = 3) }").count == 1)
    }

    @Test func aDefaultOnALaterLineOfASignatureIsAFindingOnThatLine() {
        let source = """
            func write(
                path: String,
                permissions: UInt32 = 0o600
            ) {}
            """
        let findings = defaults(in: source)
        #expect(findings.count == 1)
        #expect(findings.first?.hasPrefix("Planted.swift:3:") == true, "\(findings)")
    }

    @Test func everyDefaultOfOneSignatureIsItsOwnFinding() {
        #expect(defaults(in: "func f(a: Int = 1, b: [Int] = [1, 2], c: String = \"x\") {}").count == 3)
    }

    @Test func aCallerContextCaptureIsNotAFinding() {
        let source = """
            func fail(_ message: String, file: StaticString = #filePath, line: UInt = #line) {}
            func note(function: String = #function, id: String = #fileID, column: Int = #column) {}
            func expect(sourceLocation: SourceLocation = #_sourceLocation) {}
            """
        #expect(defaults(in: source) == [])
    }

    @Test func aCaptureMixedWithARealDefaultStillFindsTheRealOne() {
        #expect(defaults(in: "func f(line: UInt = #line, retries: Int = 3) {}").count == 1)
    }

    @Test func whatOnlyLooksLikeADefaultIsNotAFinding() {
        let source = """
            func same(a: Int, b: Int) -> Bool { a == b }
            func compare(_ a: Int, _ b: Int) -> Bool { let c = a; return c >= b }
            enum Raw: Int { case one = 1, two = 2 }
            func describe(_ value: Raw) -> String {
                switch value {
                case .one: return "one"
                case .two: return "two"
                }
            }
            func unwrap(_ optional: Int?) -> Int {
                if case .some(let value) = optional { return value }
                return call(with: 3, named: "x")
            }
            let closure: (Int) -> Int = { $0 }
            // func commented(out: Int = 1) {}
            let text = "func quoted(inAString: Int = 1) {}"
            """
        #expect(defaults(in: source) == [])
    }

    // MARK: - Reaches: planted

    private func reaches(in source: String, path: String) -> [String] {
        SourceScan.reachFindings(source: source, path: path)
    }

    private let anySourceFile = "Sources/BCFileSystem/Planted.swift"
    private let anyTestFile = "Tests/BCFileSystemTests/PlantedTests.swift"

    @Test func eachReachIsAFinding() {
        let planted = [
            "let exists = FileManager.default.fileExists(atPath: path)",
            "let manager = FileManager()",
            "let directory = NSTemporaryDirectory()",
            "let data = try Data(contentsOf: url)",
            "let text = try String(contentsOfFile: path)",
            "try data.write(to: url)",
            "let variables = ProcessInfo.processInfo.environment",
            "let home = getenv(\"HOME\")",
            "let arguments = CommandLine.arguments",
            "let now = Date()",
            "let now = Date.now",
            "try await Task.sleep(nanoseconds: 1)",
            "let id = UUID()",
            "let pick = Int.random(in: 0..<10)",
            "let session = URLSession.shared",
            "let defaults = UserDefaults.standard",
            "let zone = TimeZone.current",
            "let bundle = Bundle.main",
            "print(\"progress\")",
            "exit(1)",
            "let stamp: Date = .now",
            "configure(session: .shared)",
        ]
        for line in planted {
            #expect(reaches(in: line, path: anySourceFile).count == 1, "in a source file: \(line)")
            #expect(reaches(in: line, path: anyTestFile).count == 1, "in a test file: \(line)")
        }
    }

    @Test func aFindingNamesTheFileTheLineAndTheReach() {
        let findings = reaches(in: "let a = 1\nlet now = Date()\n", path: anySourceFile)
        #expect(findings.count == 1)
        #expect(findings.first?.hasPrefix("Sources/BCFileSystem/Planted.swift:2: reach: Date()") == true, "\(findings)")
    }

    @Test func aReachInACommentOrAStringIsNotAFinding() {
        let source = """
            // FileManager.default is what this module does not use.
            /* Nor Date(), nor UUID(). */
            let advice = "call Date() at the composition root"
            let raw = #"FileManager.default"#
            """
        #expect(reaches(in: source, path: anySourceFile) == [])
    }

    @Test func aReachInsideAStringInterpolationIsAFinding() {
        #expect(reaches(in: #"let line = "started at \(Date())""#, path: anySourceFile).count == 1)
    }

    @Test func whatOnlyLooksLikeAReachIsNotAFinding() {
        let source = """
            let stamp = Date(timeIntervalSince1970: 0)
            let id = UUID(uuidString: text)
            let zone = TimeZone(identifier: "UTC")
            let value = settings.current
            switch kind { default: break }
            let blueprint = imprint(page)
            """
        #expect(reaches(in: source, path: anySourceFile) == [])
    }

    // MARK: - The marker: planted

    // Composed from the scan's own marker, so this file never spells a marker on a line of its own.
    private let markedReach =
        "let root = NSTemporaryDirectory()  // \(SourceScan.reachMarker) where a test's own directory is created"

    @Test func aMarkedReachInTheLegalFileIsNotAFinding() {
        #expect(reaches(in: markedReach, path: SourceScan.reachIsLegalInFileEndingWith) == [])
    }

    @Test func anUnmarkedReachInTheLegalFileIsStillAFinding() {
        let findings = reaches(in: "let root = NSTemporaryDirectory()", path: SourceScan.reachIsLegalInFileEndingWith)
        #expect(findings.count == 1)
    }

    /// A marker added to make a finding pass, anywhere but the one legal file, is two findings: the reach it
    /// did not excuse, and itself.
    @Test func aMarkerAnywhereElseExcusesNothingAndIsItselfAFinding() {
        for path in [anySourceFile, anyTestFile] {
            let findings = reaches(in: markedReach, path: path)
            #expect(findings.count == 2, "\(path): \(findings)")
            #expect(findings.contains { $0.contains("reach marker") }, "\(findings)")
            #expect(findings.contains { $0.contains("reach: NSTemporaryDirectory()") }, "\(findings)")
        }
    }

    @Test func aMarkerWithoutAReasonExcusesNothing() {
        let source = "let root = NSTemporaryDirectory()  // \(SourceScan.reachMarker)"
        let findings = reaches(in: source, path: SourceScan.reachIsLegalInFileEndingWith)
        #expect(findings.count == 2, "\(findings)")
        #expect(findings.contains { $0.contains("needs a reason") }, "\(findings)")
    }

    @Test func markerTextThatIsNotAMarkerCommentIsIgnored() {
        let source = """
            /// A reach carries a comment such as `// \(SourceScan.reachMarker) the reason` on its line.
            let example = "let now = Date()  // \(SourceScan.reachMarker) quoted, not code"
            """
        #expect(reaches(in: source, path: anySourceFile) == [])
    }

    // MARK: - Macros: planted

    private func macros(in source: String) -> [String] {
        SourceScan.macroDeclarationFindings(source: source, path: "Planted.swift")
    }

    @Test func aMacroDeclarationIsAFinding() {
        let source = """
            @freestanding(expression)
            public macro stringify<T>(_ value: T) -> (T, String) = #externalMacro(module: "M", type: "S")
            """
        let findings = macros(in: source)
        #expect(findings.count == 1)
        #expect(findings.first?.hasPrefix("Planted.swift:2: macro declaration") == true, "\(findings)")
    }

    @Test func whatOnlyLooksLikeAMacroDeclarationIsNotAFinding() {
        let source = """
            // A macro declaration is forbidden; `public macro stringify` would be one.
            let note = "macro stringify"
            #expect(value == 1)
            let expanded = #externalMacro(module: "M", type: "S")
            let macroCount = 0
            """
        #expect(macros(in: source) == [])
    }

    // MARK: - The two views

    @Test func theViewsKeepEveryLineAndColumn() {
        let source = """
            let a = "text" // note
            /* block
               comment */ let b = 2
            """
        let views = SourceScan.views(of: source)
        #expect(views.code.count == source.count)
        #expect(views.lineComments.count == source.count)
        let codeLines = views.code.components(separatedBy: "\n")
        let commentLines = views.lineComments.components(separatedBy: "\n")
        #expect(codeLines.count == 3)
        // The string literal and the comment are blanked out of the code; the code is blanked out of the comments.
        #expect(codeLines[0] == "let a =" + String(repeating: " ", count: 15))
        #expect(commentLines[0] == String(repeating: " ", count: 15) + "// note")
        #expect(codeLines[2] == String(repeating: " ", count: 13) + " let b = 2")
    }
}
