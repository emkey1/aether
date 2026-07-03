//
//  MotePadDocumentStore.h
//  iSH-AOK
//
//  Guest-filesystem text I/O for the MotePad Workspace applet. Reads/writes/lists
//  files through the emulated VFS (borrowing pid 1 as `current`, exactly like
//  AudioLibrary) with an NSFileManager fast path for the realfs /AOK/persist mount.
//  Kept in its own translation unit so the kernel C headers never mix into the big
//  UIKit WorkspaceViewController.m.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, MotePadEntryType) {
    MotePadEntryTypeFile,
    MotePadEntryTypeDirectory,
    MotePadEntryTypeOther,
};

@interface MotePadDirectoryEntry : NSObject
@property (nonatomic, copy) NSString *name;       // last path component
@property (nonatomic, copy) NSString *guestPath;  // absolute guest path
@property (nonatomic) MotePadEntryType type;
@end

@interface MotePadDocumentStore : NSObject

+ (instancetype)sharedStore;

// Directory listing (directories first, then files, each sorted case-insensitively).
// Returns nil if the directory can't be opened.
- (nullable NSArray<MotePadDirectoryEntry *> *)listDirectoryAtGuestPath:(NSString *)guestPath;

// Read a file's contents as text. Tries UTF-8, then falls back to a lossy decode so
// a non-UTF-8 file still opens (degrades gracefully). Returns nil + error on failure.
- (nullable NSString *)readTextFileAtGuestPath:(NSString *)guestPath error:(NSError **)error;

// Write text as UTF-8 to a guest path, creating/truncating the file. NO + error on failure.
- (BOOL)writeText:(NSString *)text toGuestPath:(NSString *)guestPath error:(NSError **)error;

// Whether a path exists, and (optionally) whether it's a directory.
- (BOOL)fileExistsAtGuestPath:(NSString *)guestPath isDirectory:(nullable BOOL *)isDirectory;

// A sensible writable starting directory for the browser: /AOK/persist when present, else "/".
- (NSString *)defaultDirectoryGuestPath;

@end

NS_ASSUME_NONNULL_END
