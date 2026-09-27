using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Text;

namespace AIMP.Protocol.Tests;

internal static class ArchitectureTests
{
    private const int MaximumBodyLines = 15;

    internal static Task LauncherBodiesAreShortAsync()
    {
        VerifyCountingContract();
        var sourceDirectory = FindLauncherSourceDirectory();
        var violations = GetCompileSourceFiles(sourceDirectory)
            .SelectMany(InspectFile)
            .OrderBy(value => value.File, StringComparer.OrdinalIgnoreCase)
            .ThenBy(value => value.Line)
            .ToArray();
        if (violations.Length != 0)
        {
            var details = string.Join(Environment.NewLine,
                violations.Select(FormatViolation));
            throw new InvalidOperationException(
                $"Launcher bodies exceed {MaximumBodyLines} nonblank lines:{Environment.NewLine}"
                + details);
        }

        return Task.CompletedTask;
    }

    private static IEnumerable<BodyViolation> InspectFile(string path)
    {
        var source = SourceText.From(File.ReadAllText(path));
        var tree = CSharpSyntaxTree.ParseText(
            source,
            new CSharpParseOptions(LanguageVersion.Latest),
            path);
        var errors = tree.GetDiagnostics().Where(value => value.Severity == DiagnosticSeverity.Error)
            .ToArray();
        if (errors.Length != 0)
        {
            throw new InvalidOperationException(
                $"Roslyn could not parse {path}:{Environment.NewLine}"
                + string.Join(Environment.NewLine, errors.Select(value => value.ToString())));
        }

        return EnumerateBodies(tree.GetRoot())
            .Select(value => InspectBody(path, source, value.Owner, value.Body))
            .Where(value => value is not null)
            .Select(value => value!);
    }

    private static IEnumerable<BodyCandidate> EnumerateBodies(SyntaxNode root)
    {
        foreach (var node in root.DescendantNodes())
        {
            var body = GetBody(node);
            if (body is not null)
            {
                yield return new BodyCandidate(node, body);
            }
        }
    }

    private static CSharpSyntaxNode? GetBody(SyntaxNode node) => node switch
    {
        BaseMethodDeclarationSyntax value =>
            (CSharpSyntaxNode?)value.Body ?? value.ExpressionBody?.Expression,
        AccessorDeclarationSyntax value =>
            (CSharpSyntaxNode?)value.Body ?? value.ExpressionBody?.Expression,
        LocalFunctionStatementSyntax value =>
            (CSharpSyntaxNode?)value.Body ?? value.ExpressionBody?.Expression,
        PropertyDeclarationSyntax value => value.ExpressionBody?.Expression,
        IndexerDeclarationSyntax value => value.ExpressionBody?.Expression,
        ParenthesizedLambdaExpressionSyntax value => value.Body,
        SimpleLambdaExpressionSyntax value => value.Body,
        AnonymousMethodExpressionSyntax value => value.Block,
        _ => null
    };

    private static BodyViolation? InspectBody(
        string path,
        SourceText source,
        SyntaxNode owner,
        CSharpSyntaxNode body)
    {
        var span = BodySpan(body);
        var count = CountNonblankLines(source.ToString(span));
        if (count <= MaximumBodyLines)
        {
            return null;
        }

        var line = source.Lines.GetLinePosition(owner.SpanStart).Line + 1;
        return new BodyViolation(path, line, Describe(owner), count);
    }

    private static TextSpan BodySpan(CSharpSyntaxNode body)
    {
        if (body is not BlockSyntax block
            || block.OpenBraceToken.IsMissing
            || block.CloseBraceToken.IsMissing)
        {
            return body.Span;
        }

        return TextSpan.FromBounds(
            block.OpenBraceToken.Span.End,
            block.CloseBraceToken.SpanStart);
    }

    private static int CountNonblankLines(string text) =>
        text.Replace("\r\n", "\n", StringComparison.Ordinal)
            .Replace('\r', '\n')
            .Split('\n')
            .Count(value => !string.IsNullOrWhiteSpace(value));

    private static string Describe(SyntaxNode node) => node switch
    {
        MethodDeclarationSyntax value => $"method {value.Identifier.ValueText}",
        ConstructorDeclarationSyntax value => $"constructor {value.Identifier.ValueText}",
        DestructorDeclarationSyntax value => $"destructor {value.Identifier.ValueText}",
        OperatorDeclarationSyntax value => $"operator {value.OperatorToken.ValueText}",
        ConversionOperatorDeclarationSyntax value =>
            $"conversion operator {value.Type}",
        AccessorDeclarationSyntax value => $"accessor {value.Keyword.ValueText}",
        LocalFunctionStatementSyntax value => $"local function {value.Identifier.ValueText}",
        PropertyDeclarationSyntax value => $"property {value.Identifier.ValueText}",
        IndexerDeclarationSyntax => "indexer",
        LambdaExpressionSyntax => "lambda",
        AnonymousMethodExpressionSyntax => "anonymous method",
        _ => node.Kind().ToString()
    };

    private static IEnumerable<string> GetCompileSourceFiles(string sourceDirectory) =>
        Directory.EnumerateFiles(sourceDirectory, "*.cs", SearchOption.AllDirectories)
            .Where(path => !IsBuildOutput(sourceDirectory, path));

    private static bool IsBuildOutput(string root, string path)
    {
        var relative = Path.GetRelativePath(root, path);
        var segments = relative.Split(
            new[] { Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar },
            StringSplitOptions.RemoveEmptyEntries);
        return segments.Any(value =>
            string.Equals(value, "bin", StringComparison.OrdinalIgnoreCase)
            || string.Equals(value, "obj", StringComparison.OrdinalIgnoreCase));
    }

    private static string FindLauncherSourceDirectory()
    {
        var starts = new[] { Directory.GetCurrentDirectory(), AppContext.BaseDirectory };
        foreach (var start in starts.Distinct(StringComparer.OrdinalIgnoreCase))
        {
            for (var current = new DirectoryInfo(start); current is not null;
                 current = current.Parent)
            {
                var direct = Path.Combine(current.FullName, "src", "AIMP.Launcher");
                if (File.Exists(Path.Combine(direct, "AIMP.Launcher.csproj"))) return direct;
                var nested = Path.Combine(current.FullName, "AIMP.Launcher", "src", "AIMP.Launcher");
                if (File.Exists(Path.Combine(nested, "AIMP.Launcher.csproj"))) return nested;
            }
        }

        throw new DirectoryNotFoundException("The AIMP.Launcher source directory was not found.");
    }

    private static void VerifyCountingContract()
    {
        const string sample = "class Sample { void Run() { // comment\n Execute(); } }";
        var root = CSharpSyntaxTree.ParseText(sample).GetRoot();
        var block = root.DescendantNodes().OfType<MethodDeclarationSyntax>().Single().Body!;
        var source = SourceText.From(sample);
        AssertEx.Equal(2, CountNonblankLines(source.ToString(BodySpan(block))),
            "Comments must count while the outer body braces remain excluded.");
    }

    private static string FormatViolation(BodyViolation value) =>
        $"{Path.GetFileName(value.File)}:{value.Line}: {value.Description} "
        + $"has {value.NonblankLines} lines";

    private sealed record BodyCandidate(SyntaxNode Owner, CSharpSyntaxNode Body);

    private sealed record BodyViolation(
        string File,
        int Line,
        string Description,
        int NonblankLines);
}
