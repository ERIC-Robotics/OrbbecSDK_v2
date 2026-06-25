## What is an SBOM?
An SBOM (Software Bill Of Materials) is a detailed list of all components, libraries, and dependencies included in a software project. It helps with security, compliance, and transparency.

## SBOM Usage and Generation Rules

- **Generate an SBOM for every release:** Ensure each software release is accompanied by an up-to-date SBOM.
- **Automate SBOM generation:** Integrate SBOM creation into your CI/CD pipeline to maintain consistency.
- **Update SBOMs on changes:** Regenerate the SBOM whenever dependencies or components change.

## References
- [Syft Documentation](https://github.com/anchore/syft)
- [CycloneDX Documentation](https://cyclonedx.org/docs/)
- [SPDX Documentation](https://spdx.dev/resources/tools/)