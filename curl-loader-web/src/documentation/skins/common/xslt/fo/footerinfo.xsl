<?xml version="1.0" encoding="UTF-8"?>
<!--
  Copyright 2002-2004 The Apache Software Foundation

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
-->
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                xmlns:fo="http://www.w3.org/1999/XSL/Format"
                version="1.0">

<!--
Named template to generate a short message in the PDF footer, from text in
skinconf.xml.  By default, the message is a copyright statement.  If a credit
with @role='pdf' is present, that is used instead.  Eg:

<credit role="pdf">
  <name>Generated by Apache FOP 1.0-dev</name>
  <url>http://xml.apache.org/fop/dev/</url>
</credit>
-->
  <xsl:template name="info">
    <xsl:variable name="disable-copyright-footer" select="//skinconfig/pdf/disable-copyright-footer"/>
    <xsl:variable name="pdfcredit" select="//skinconfig/credits/credit[@role = 'pdf']"/>
    <xsl:variable name="text">
      <xsl:if test="$pdfcredit">
        <xsl:value-of select="$pdfcredit/name"/>
      </xsl:if>
      <xsl:if test="not($pdfcredit) and not($disable-copyright-footer = 'true')">
        <xsl:text>Copyright &#169; </xsl:text><xsl:value-of select="//skinconfig/year"/>&#160;<xsl:value-of
          select="//skinconfig/vendor"/><xsl:text> All rights reserved.</xsl:text>
      </xsl:if>
    </xsl:variable>
    <xsl:variable name="url" select="$pdfcredit/url"/>

    <fo:block-container font-style="italic" absolute-position="absolute"
      left="0pt" top="0pt" right="6.25in" bottom="150pt"
      font-size="10pt">
      <xsl:if test="not($url)">
        <fo:block text-align="center" color="lightgrey">
          <xsl:value-of select="$text"/>
        </fo:block>
      </xsl:if>
      <xsl:if test="$url">
        <fo:block text-align="center">
          <fo:basic-link color="lightgrey"
            external-destination="{$url}">
            <xsl:value-of select="$text"/>
          </fo:basic-link>
        </fo:block>
        <fo:block text-align="center">
          <fo:basic-link color="lightgrey"
            external-destination="{$url}">
            <xsl:value-of select="$url"/>
          </fo:basic-link>
        </fo:block>
      </xsl:if>
    </fo:block-container>
  </xsl:template>

</xsl:stylesheet>
