//
//  MarkdownRenderer.h
//  iSH-AOK
//
//  A small, dependency-free Markdown -> NSAttributedString renderer, shared by
//  the LLM chat transcript and the Workspace markdown reader applet.
//  NSAttributedString's own initWithMarkdown... is iOS 15+ and inline only
//  (no headings/lists/code blocks), and the app targets iOS 12, so this
//  handles the block- and inline-level constructs Markdown documents and LLM
//  replies actually use: headings, bold/italic, inline code, fenced code
//  blocks, blockquotes, bullet/numbered lists, links, and horizontal rules.
//

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

// Applies `add` on top of font's existing symbolic traits (e.g. bold a font
// that's already italic without losing the italic).
UIFont *ISHMarkdownFontWithTraits(UIFont *font, UIFontDescriptorSymbolicTraits add);

// Renders `markdown` with the given palette. `codeBg` may be nil (no code
// block / inline-code background fill).
NSAttributedString *ISHMarkdownAttributedStringFromMarkdown(NSString *markdown,
                                                              UIFont *baseFont,
                                                              UIColor *textColor,
                                                              UIColor *secondaryColor,
                                                              UIColor * _Nullable codeBg,
                                                              UIColor *linkColor);

NS_ASSUME_NONNULL_END
