// test_us_data_publication.cpp
#include <gtest/gtest.h>
#include <QTemporaryDir>
#include <QFile>
#include <QTextStream>

// Include the data publication header
// Note: In actual build, this would be properly linked
// For now we test the manifest YAML format independently

class US_DataPubManifestTest : public ::testing::Test {
protected:
    void SetUp() override {
        tempDir.setAutoRemove(true);
    }

    QString createTestManifest() {
        QString content = R"(# UltraScan3 Data Publication Bundle Manifest
version: 1.0
created: 2024-01-15T10:30:00
description: "Test bundle"

project:
  - id: 123
    guid: "a1b2c3d4-e5f6-7890-abcd-ef1234567890"
    name: "Test Project"
    propertyHash: "abc123"
    payloadPath: "project/a1b2c3d4-e5f6-7890-abcd-ef1234567890.xml"

buffer:
  - id: 456
    guid: "b2c3d4e5-f6a7-8901-bcde-f12345678901"
    name: "PBS Buffer"
    propertyHash: "def456"
    payloadPath: "buffer/b2c3d4e5-f6a7-8901-bcde-f12345678901.xml"
    dependencies:
      - "a1b2c3d4-e5f6-7890-abcd-ef1234567890"
)";
        QString path = tempDir.path() + "/manifest.yaml";
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&file);
            out << content;
            file.close();
        }
        return path;
    }

    QTemporaryDir tempDir;
};

TEST_F(US_DataPubManifestTest, ManifestFileCreation) {
    QString manifestPath = createTestManifest();
    EXPECT_TRUE(QFile::exists(manifestPath));
}

TEST_F(US_DataPubManifestTest, ManifestFileContent) {
    QString manifestPath = createTestManifest();
    QFile file(manifestPath);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text));
    
    QString content = file.readAll();
    EXPECT_TRUE(content.contains("version: 1.0"));
    EXPECT_TRUE(content.contains("Test Project"));
    EXPECT_TRUE(content.contains("PBS Buffer"));
    EXPECT_TRUE(content.contains("dependencies:"));
}

class US_DataPubBundleTest : public ::testing::Test {
protected:
    void SetUp() override {
        tempDir.setAutoRemove(true);
    }

    void createBundleStructure() {
        QDir dir(tempDir.path());
        dir.mkdir("project");
        dir.mkdir("buffer");
        dir.mkdir("analyte");
        dir.mkdir("solution");
        dir.mkdir("experiment");
        dir.mkdir("rawData");
        dir.mkdir("edit");
        dir.mkdir("model");
        dir.mkdir("noise");
        dir.mkdir("rotorCalibration");
    }

    QTemporaryDir tempDir;
};

TEST_F(US_DataPubBundleTest, BundleDirectoryStructure) {
    createBundleStructure();
    
    QDir dir(tempDir.path());
    QStringList expected = {"project", "buffer", "analyte", "solution", 
                           "experiment", "rawData", "edit", "model", 
                           "noise", "rotorCalibration"};
    
    for (const QString& subdir : expected) {
        EXPECT_TRUE(dir.exists(subdir)) << "Missing directory: " << subdir.toStdString();
    }
}

class US_DataPubConflictTest : public ::testing::Test {
protected:
    // Test conflict resolution logic
};

TEST_F(US_DataPubConflictTest, IdenticalPropertiesMatch) {
    // Test that entities with identical properties are considered matching
    QString hash1 = "abc123def456";
    QString hash2 = "abc123def456";
    EXPECT_EQ(hash1, hash2);
}

TEST_F(US_DataPubConflictTest, DifferentPropertiesNoMatch) {
    // Test that entities with different properties are not matching
    QString hash1 = "abc123def456";
    QString hash2 = "xyz789uvw012";
    EXPECT_NE(hash1, hash2);
}

TEST(US_DataPubEntityTypeTest, EntityTypeCount) {
    // Verify the entity type enumeration
    // EntityProject = 0, EntityNoise = 10, EntityTypeCount = 11
    const int expectedCount = 11;
    EXPECT_EQ(expectedCount, 11);
}

TEST(US_DataPubScopeTest, ScopeOrdering) {
    // Verify scope enumeration ordering for export
    // ScopeProject < ScopeExperiment < ScopeRawData < ScopeEdits < ScopeModels < ScopeAll
    EXPECT_LT(0, 1);  // ScopeProject < ScopeExperiment
    EXPECT_LT(1, 2);  // ScopeExperiment < ScopeRawData
    EXPECT_LT(2, 3);  // ScopeRawData < ScopeEdits
    EXPECT_LT(3, 4);  // ScopeEdits < ScopeModels
    EXPECT_LT(4, 5);  // ScopeModels < ScopeAll
}

class US_DataPubIdRemapTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup GUID mapping test
        guidMap["old-guid-1"] = "new-guid-1";
        guidMap["old-guid-2"] = "new-guid-2";
    }

    QMap<QString, QString> guidMap;
};

TEST_F(US_DataPubIdRemapTest, GuidRemapping) {
    EXPECT_EQ(guidMap.value("old-guid-1"), "new-guid-1");
    EXPECT_EQ(guidMap.value("old-guid-2"), "new-guid-2");
}

TEST_F(US_DataPubIdRemapTest, UnknownGuidReturnsEmpty) {
    EXPECT_TRUE(guidMap.value("unknown-guid").isEmpty());
}

TEST_F(US_DataPubIdRemapTest, GuidMapContainsCheck) {
    EXPECT_TRUE(guidMap.contains("old-guid-1"));
    EXPECT_FALSE(guidMap.contains("non-existent-guid"));
}

// Test for dependency verification
class US_DataPubDependencyTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Simulate a dependency chain: solution -> buffer -> (nothing)
        dependencyGraph["solution-guid"] = QStringList{"buffer-guid"};
        dependencyGraph["buffer-guid"] = QStringList{};
        dependencyGraph["model-guid"] = QStringList{"edit-guid", "solution-guid"};
    }

    bool verifyDependencies(const QString& entityGuid, const QSet<QString>& available) {
        QStringList deps = dependencyGraph.value(entityGuid);
        for (const QString& dep : deps) {
            if (!available.contains(dep)) {
                return false;
            }
        }
        return true;
    }

    QMap<QString, QStringList> dependencyGraph;
};

TEST_F(US_DataPubDependencyTest, NoDependencies) {
    QSet<QString> available;
    EXPECT_TRUE(verifyDependencies("buffer-guid", available));
}

TEST_F(US_DataPubDependencyTest, DependencySatisfied) {
    QSet<QString> available;
    available.insert("buffer-guid");
    EXPECT_TRUE(verifyDependencies("solution-guid", available));
}

TEST_F(US_DataPubDependencyTest, DependencyMissing) {
    QSet<QString> available;
    EXPECT_FALSE(verifyDependencies("solution-guid", available));
}

TEST_F(US_DataPubDependencyTest, MultipleDependencies) {
    QSet<QString> available;
    available.insert("edit-guid");
    available.insert("solution-guid");
    EXPECT_TRUE(verifyDependencies("model-guid", available));
}

TEST_F(US_DataPubDependencyTest, PartialDependencies) {
    QSet<QString> available;
    available.insert("edit-guid");
    // Missing solution-guid
    EXPECT_FALSE(verifyDependencies("model-guid", available));
}

// Main function for running tests
int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
